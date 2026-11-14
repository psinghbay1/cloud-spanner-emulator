//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/query/queryable_table.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "backend/access/read.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/catalog.h"
#include "backend/query/queryable_column.h"
#include "tests/common/row_cursor.h"
#include "tests/common/row_reader.h"
#include "tests/common/schema_constructor.h"
#include "common/config.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using testing::ElementsAre;

class QueryableTableTest : public testing::Test {
 public:
  const Schema* schema() { return schema_.get(); }
  RowReader* reader() { return &reader_; }

 private:
  googlesql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_ =
      test::CreateSchemaWithOneTable(&type_factory_);
  test::TestRowReader reader_{
      {{"test_table",
        {{"int64_col", "string_col"},
         {googlesql::types::Int64Type(), googlesql::types::StringType()},
         {{googlesql::values::Int64(42), googlesql::values::String("foo")}}}}}};
};

TEST_F(QueryableTableTest, FindColumnByName) {
  const auto* schema_table = schema()->FindTable("test_table");
  QueryableTable table{schema_table, reader()};
  const auto* column = table.FindColumnByName("string_col");
  EXPECT_NE(column, nullptr);
  const QueryableColumn* queryable_column =
      dynamic_cast<const QueryableColumn*>(column);
  EXPECT_NE(queryable_column, nullptr);
  EXPECT_EQ(queryable_column->wrapped_column(),
            schema_table->FindColumn("string_col"));
}

TEST_F(QueryableTableTest, PrimaryKey) {
  const auto* schema_table = schema()->FindTable("test_table");
  QueryableTable table{schema_table, reader()};
  ASSERT_TRUE(table.PrimaryKey().has_value());
  EXPECT_THAT(table.PrimaryKey().value(),
              ElementsAre(0 /* index of int64_col*/));
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithZeroColumns) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{}).value();
  ASSERT_EQ(iterator->NumColumns(), 0);
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  ASSERT_FALSE(iterator->NextRow());
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithTheSecondColumn) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{1}).value();
  ASSERT_EQ(iterator->NumColumns(), 1);
  EXPECT_EQ(iterator->GetColumnName(0), "string_col");
  EXPECT_TRUE(iterator->GetColumnType(0)->IsString());
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  EXPECT_EQ(iterator->GetValue(0).string_value(), "foo");
  ASSERT_FALSE(iterator->NextRow());
}

TEST_F(QueryableTableTest, CreateEvaluatorTableIteratorWithAllColumns) {
  QueryableTable table{schema()->FindTable("test_table"), reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{0, 1}).value();
  ASSERT_EQ(iterator->NumColumns(), 2);
  EXPECT_EQ(iterator->GetColumnName(0), "int64_col");
  EXPECT_EQ(iterator->GetColumnName(1), "string_col");
  EXPECT_TRUE(iterator->GetColumnType(0)->IsInt64());
  EXPECT_TRUE(iterator->GetColumnType(1)->IsString());
  ASSERT_TRUE(iterator->NextRow());
  GOOGLESQL_ASSERT_OK(iterator->Status());
  EXPECT_EQ(iterator->GetValue(0).int64_value(), 42);
  EXPECT_EQ(iterator->GetValue(1).string_value(), "foo");
  ASSERT_FALSE(iterator->NextRow());
}


// Records every read so a test can check the key set, and so the lock, that a
// scan asks for. Rows come from a TestRowReader, which ignores the key set.
class RecordingRowReader : public RowReader {
 public:
  explicit RecordingRowReader(test::TestRowReader* rows) : rows_(rows) {}

  absl::Status Read(const ReadArg& read_arg,
                    std::unique_ptr<RowCursor>* cursor) override {
    reads_.push_back(read_arg);
    return rows_->Read(read_arg, cursor);
  }

  const std::vector<ReadArg>& reads() const { return reads_; }

 private:
  test::TestRowReader* rows_;
  std::vector<ReadArg> reads_;
};

using ColumnFilters =
    absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>;

class QueryableTablePushdownTest : public testing::Test {
 public:
  void SetUp() override {
    saved_pushdown_ = config::enable_sql_read_pushdown();
    config::set_enable_sql_read_pushdown(true);
    schema_ = test::CreateSchemaFromDDL(
                  {R"(
                    CREATE TABLE Accounts (
                      id INT64 NOT NULL,
                      status STRING(MAX)
                    ) PRIMARY KEY (id)
                  )",
                   R"(
                    CREATE TABLE Entries (
                      account_id INT64 NOT NULL,
                      entry_id INT64 NOT NULL,
                      amount INT64
                    ) PRIMARY KEY (account_id, entry_id)
                  )",
                   "CREATE INDEX AccountsByStatus ON Accounts(status)"},
                  &type_factory_)
                  .value();
  }

  void TearDown() override {
    config::set_enable_sql_read_pushdown(saved_pushdown_);
  }

  // Opens a scan of every column of `table_name`, hands it `filters` as the
  // evaluator would, and returns the key set of the scan's base-table read.
  KeySet ScanKeySet(const std::string& table_name, ColumnFilters filters,
                    const Index* forced_index = nullptr) {
    QueryableTable table{schema_->FindTable(table_name), &reader_};
    if (forced_index != nullptr) {
      table.RecordForcedIndex(forced_index);
    }
    std::vector<int> column_idxs;
    for (int i = 0; i < table.NumColumns(); ++i) {
      column_idxs.push_back(i);
    }
    auto iterator = table.CreateEvaluatorTableIterator(column_idxs).value();
    GOOGLESQL_EXPECT_OK(iterator->SetColumnFilterMap(std::move(filters)));
    while (iterator->NextRow()) {
    }
    GOOGLESQL_EXPECT_OK(iterator->Status());
    EXPECT_FALSE(reader_.reads().empty());
    return reader_.reads().back().key_set;
  }

  static std::unique_ptr<googlesql::ColumnFilter> Equals(int64_t value) {
    return std::make_unique<googlesql::ColumnFilter>(
        googlesql::values::Int64(value), googlesql::values::Int64(value));
  }

  const Schema* schema() { return schema_.get(); }
  RecordingRowReader& reader() { return reader_; }

 private:
  bool saved_pushdown_ = false;
  googlesql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_;
  test::TestRowReader rows_{
      {{"Accounts",
        {{"id", "status"},
         {googlesql::types::Int64Type(), googlesql::types::StringType()},
         {{googlesql::values::Int64(1), googlesql::values::String("open")},
          {googlesql::values::Int64(2), googlesql::values::String("closed")}}}},
       {"Entries",
        {{"account_id", "entry_id", "amount"},
         {googlesql::types::Int64Type(), googlesql::types::Int64Type(),
          googlesql::types::Int64Type()},
         {}}}}};
  RecordingRowReader reader_{&rows_};
};

std::string Expected(const KeySet& key_set) { return key_set.DebugString(); }

TEST_F(QueryableTablePushdownTest, EqualityOnFullKeyReadsOnePoint) {
  ColumnFilters filters;
  filters[0] = Equals(1);
  KeySet expected;
  expected.AddKey(Key({googlesql::values::Int64(1)}));
  EXPECT_EQ(ScanKeySet("Accounts", std::move(filters)).DebugString(),
            Expected(expected));
}

TEST_F(QueryableTablePushdownTest, InListOnFullKeyReadsEachPoint) {
  ColumnFilters filters;
  filters[0] = std::make_unique<googlesql::ColumnFilter>(
      std::vector<googlesql::Value>{googlesql::values::Int64(1),
                                    googlesql::values::Int64(3)});
  KeySet expected;
  expected.AddKey(Key({googlesql::values::Int64(1)}));
  expected.AddKey(Key({googlesql::values::Int64(3)}));
  EXPECT_EQ(ScanKeySet("Accounts", std::move(filters)).DebugString(),
            Expected(expected));
}

TEST_F(QueryableTablePushdownTest, EqualityOnLeadingKeyColumnReadsAPrefix) {
  ColumnFilters filters;
  filters[0] = Equals(7);
  KeySet expected;
  expected.AddRange(KeyRange::Prefix(Key({googlesql::values::Int64(7)})));
  EXPECT_EQ(ScanKeySet("Entries", std::move(filters)).DebugString(),
            Expected(expected));
}

TEST_F(QueryableTablePushdownTest, RangeAfterAPrefixReadsAClosedRange) {
  ColumnFilters filters;
  filters[0] = Equals(7);
  filters[1] = std::make_unique<googlesql::ColumnFilter>(
      googlesql::values::Int64(2), googlesql::values::Int64(5));
  KeySet expected;
  expected.AddRange(KeyRange::ClosedClosed(
      Key({googlesql::values::Int64(7), googlesql::values::Int64(2)}),
      Key({googlesql::values::Int64(7), googlesql::values::Int64(5)})));
  EXPECT_EQ(ScanKeySet("Entries", std::move(filters)).DebugString(),
            Expected(expected));
}

TEST_F(QueryableTablePushdownTest, NonKeyFilterReadsTheWholeTable) {
  ColumnFilters filters;
  filters[1] = std::make_unique<googlesql::ColumnFilter>(
      googlesql::values::String("open"), googlesql::values::String("open"));
  EXPECT_EQ(ScanKeySet("Accounts", std::move(filters)).DebugString(),
            Expected(KeySet::All()));
}

TEST_F(QueryableTablePushdownTest, NoFilterReadsTheWholeTable) {
  EXPECT_EQ(ScanKeySet("Accounts", ColumnFilters()).DebugString(),
            Expected(KeySet::All()));
}

TEST_F(QueryableTablePushdownTest, ForcedIndexReadsIndexRangeThenBaseRows) {
  const Index* index = schema()->FindIndex("AccountsByStatus");
  ASSERT_NE(index, nullptr);
  ColumnFilters filters;
  filters[1] = std::make_unique<googlesql::ColumnFilter>(
      googlesql::values::String("open"), googlesql::values::String("open"));
  KeySet base_keys = ScanKeySet("Accounts", std::move(filters), index);

  ASSERT_EQ(reader().reads().size(), 2);
  const ReadArg& index_read = reader().reads().front();
  EXPECT_EQ(index_read.index, "AccountsByStatus");
  KeySet expected_index_range;
  expected_index_range.AddRange(
      KeyRange::Prefix(Key({googlesql::values::String("open")})));
  EXPECT_EQ(index_read.key_set.DebugString(), Expected(expected_index_range));
  // The test reader ignores the index range and returns both rows, so both
  // base keys come back as points.
  KeySet expected_base_keys;
  expected_base_keys.AddKey(Key({googlesql::values::Int64(1)}));
  expected_base_keys.AddKey(Key({googlesql::values::Int64(2)}));
  EXPECT_EQ(base_keys.DebugString(), Expected(expected_base_keys));
}

TEST_F(QueryableTablePushdownTest, ForcedIndexWithoutIndexKeyFilterFallsBack) {
  const Index* index = schema()->FindIndex("AccountsByStatus");
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(ScanKeySet("Accounts", ColumnFilters(), index).DebugString(),
            Expected(KeySet::All()));
  EXPECT_EQ(reader().reads().size(), 1);
}

TEST_F(QueryableTablePushdownTest, FlagOffReadsTheWholeTableAtOnce) {
  config::set_enable_sql_read_pushdown(false);
  // With the flag off the read happens while the iterator is created, before
  // any filter is known.
  QueryableTable table{schema()->FindTable("Accounts"), &reader()};
  auto iterator =
      table.CreateEvaluatorTableIterator(/*column_idxs=*/{0, 1}).value();
  ASSERT_EQ(reader().reads().size(), 1);
  EXPECT_EQ(reader().reads().front().key_set.DebugString(),
            Expected(KeySet::All()));
  ColumnFilters filters;
  filters[0] = Equals(1);
  GOOGLESQL_EXPECT_OK(iterator->SetColumnFilterMap(std::move(filters)));
  EXPECT_EQ(reader().reads().size(), 1);
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
