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

#include "backend/storage/in_memory_storage.h"

#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Bool;
using googlesql::values::Int64;
using googlesql::values::String;

class InMemoryStorageTest : public testing::Test {
 protected:
  const TableID kTableId0 = "test_table:0";
  const TableID kTableId1 = "test_table:1";
  const ColumnID kColumnID = "test_column:0";
  const KeyRange kKeyRange0To5 =
      KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(5)}));
  InMemoryStorage storage_;
  std::unique_ptr<StorageIterator> itr_;
};

TEST_F(InMemoryStorageTest, LookupByTable) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                           {String("value-10")}));

  // Read from the 2 tables.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-10")));
}

TEST_F(InMemoryStorageTest, ReadByTable) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(4)}), {kColumnID},
                           {String("value-10")}));

  // Read from the 2 table.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->ColumnValue(0), String("value-1"));
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
  EXPECT_FALSE(itr_->Next());

  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId1, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->ColumnValue(0), String("value-10"));
  EXPECT_EQ(itr_->Key(), Key({Int64(4)}));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadRangeFromSingleTable) {
  absl::Time t0 = absl::Now();

  // Write into table.
  for (int i = 0; i < 5; ++i) {
    GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(i)}), {kColumnID},
                             {String(absl::StrCat("value-", i))}));
  }

  // Read from the table.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->ColumnValue(0), String(absl::StrCat("value-", i)));
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupByTimestamp) {
  absl::Time write_ts = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  // Lookup key at exact timestamp it was written.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                            &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));

  // Lookup key at a future timestamp.
  absl::Time lookup_in_future_ts = write_ts + absl::Nanoseconds(24);
  GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_in_future_ts, kTableId0, Key({Int64(1)}),
                            {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-1")));

  // Lookup key at timestamp before the first time it was written.
  absl::Time lookup_before_write_ts = write_ts - absl::Nanoseconds(1);
  EXPECT_THAT(storage_.Lookup(lookup_before_write_ts, kTableId0,
                              Key({Int64(1)}), {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_TRUE(values.empty());
}

TEST_F(InMemoryStorageTest, ReadByTimestamp) {
  absl::Time write_ts = absl::Now();

  // Write into table.
  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  // Read key at the exact timestamp it was written.
  GOOGLESQL_EXPECT_OK(
      storage_.Read(write_ts, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), String("value-1"));
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));

  // Read key at a future timestamp.
  absl::Time read_in_future_ts = write_ts + absl::Nanoseconds(24);
  GOOGLESQL_EXPECT_OK(storage_.Read(read_in_future_ts, kTableId0, kKeyRange0To5,
                          {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), String("value-1"));
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));

  // Read key at timestamp before the first time it was written.
  absl::Time read_before_write_ts = write_ts - absl::Nanoseconds(1);
  GOOGLESQL_EXPECT_OK(storage_.Read(read_before_write_ts, kTableId0, kKeyRange0To5,
                          {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupInvalidTableReturnsNotFoundError) {
  absl::Time t0 = absl::Now();

  // Lookup a table_id in empty storage.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // Lookup invalid table_id in storage.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(storage_.Lookup(t0, "invalid-table_id_", Key({Int64(1)}),
                              {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, ReadInvalidTableReturnsEmptyResult) {
  absl::Time t0 = absl::Now();

  // Read a table_id in empty storage.
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());

  // Read invalid table_id in storage.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, "invalid-table_id_", kKeyRange0To5, {kColumnID},
                          &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupMissingKeyReturnsNotFound) {
  absl::Time t0 = absl::Now();

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(100)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, ReadMissingKeyReturnsEmptyItr) {
  absl::Time t0 = absl::Now();
  KeyRange key_range = KeyRange::ClosedOpen(Key({Int64(10)}), Key({Int64(50)}));

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, key_range, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadEmptyKeyRangeReturnsEmptyItr) {
  absl::Time t0 = absl::Now();
  KeyRange key_range = KeyRange::Empty();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, key_range, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());

  GOOGLESQL_EXPECT_OK(storage_.Read(
      t0, kTableId0,
      KeyRange(EndpointType::kClosed, Key(), EndpointType::kOpen, Key()), {},
      &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupByMissingColumnReturnsInvalidValues) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(t0 + absl::Nanoseconds(5), kTableId0,
                            Key({Int64(1)}), {"invalid_kColumnID"}, &values));
  EXPECT_FALSE(values.empty());
  EXPECT_FALSE(values[0].is_valid());
}

TEST_F(InMemoryStorageTest, ReadByMissingColumnReturnsInvalidValues) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  GOOGLESQL_EXPECT_OK(storage_.Read(t0, kTableId0, kKeyRange0To5, {"invalid_kColumnID"},
                          &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_FALSE(itr_->ColumnValue(0).is_valid());
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
}

TEST_F(InMemoryStorageTest, LookupWithoutColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {}, &values));
  EXPECT_TRUE(values.empty());
}

TEST_F(InMemoryStorageTest, ReadWithoutColumns) {
  absl::Time write_ts = absl::Now();
  absl::Time lookup_ts = write_ts + absl::Seconds(1);
  Key key({String("key"), Int64(1)});

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  std::unique_ptr<StorageIterator> itr;
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::Point(key), {}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 0);
  EXPECT_EQ(itr_->Key(), key);
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupWithNullValuesReturnsInternalError) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  EXPECT_THAT(storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                              /*values =*/nullptr),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, WriteWithEmptyKeyAndColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key(), {}, {}));
}

TEST_F(InMemoryStorageTest, WriteWithEmptyColumns) {
  absl::Time t0 = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {}, {}));
}

TEST_F(InMemoryStorageTest, DeleteFromNonExistentTable) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Delete(t0, kTableId0, KeyRange::Point(key)));
}

TEST_F(InMemoryStorageTest, DuplicateDeleteReturnsOk) {
  Key key({Int64(1)});
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(
      storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
}

TEST_F(InMemoryStorageTest, DeleteEmptyRangeWillDeleteNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = absl::Now();
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange()));
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                            &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

TEST_F(InMemoryStorageTest, DeleteLargeRangeFromSparseTable) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write sparse keys.
  for (int j = 0; j < 5; j++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(5 * j)}),
                             {kColumnID}, {Bool(true)}));
  }

  // Delete key range [0, 50).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(50)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  for (int j = 0; j < 5; j++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(5 * j)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
}

TEST_F(InMemoryStorageTest, DeleteRangeNotInTableWillDeleteNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write key range [0, 5).
  for (int i = 0; i < 5; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(i)}), {kColumnID},
                             {Bool(true)}));
  }

  // Delete key range [10, 100).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(10)}), Key({Int64(100)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  // Lookup for all existing keys [0, 5) should succeed.
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
}

TEST_F(InMemoryStorageTest, DeletePartialKeyRangeFromTable) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  // Write keys in the range [10, 15].
  for (int i = 10; i <= 15; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(i)}), {kColumnID},
                             {Bool(true)}));
  }

  // Delete keys in the range [6, 12).
  KeyRange key_range(KeyRange::ClosedOpen(Key({Int64(6)}), Key({Int64(12)})));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, key_range));

  // Lookup after delete should return invalid values for keys range [10, 12).
  std::vector<googlesql::Value> values;
  for (int i = 10; i < 12; i++) {
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Lookup for range [12, 15] should return valid values.
  for (int i = 12; i <= 15; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
}

TEST_F(InMemoryStorageTest,
       DeleteUsingInvalidKeyRangeEndpointsReturnsInternalError) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, key, {}, {}));
  EXPECT_THAT(storage_.Delete(t0, kTableId0,
                              KeyRange::OpenClosed(Key({Int64(0)}), key)),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Delete(t0, kTableId0,
                      KeyRange::OpenOpen(Key({Int64(0)}), Key({Int64(2)}))),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(storage_.Delete(t0, kTableId0,
                              KeyRange::ClosedClosed(Key({Int64(0)}), key)),
              googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, DeleteUsingEmptyKeyRangeDeletesNothing) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  // Explicit Empty KeyRange.
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Empty()));

  // StartKey <= EndKey is considered an empty range.
  GOOGLESQL_EXPECT_OK(
      storage_.Delete(delete_ts, kTableId0,
                      KeyRange::ClosedOpen(Key({Int64(5)}), Key({Int64(0)}))));
  // Lookup keys in the range.
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                              {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
    values.clear();
  }
  // Read
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, DeleteUsingAllKeyRangeDeletesEverything) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::All()));
  // Lookup
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(storage_.Lookup(lookup_ts, kTableId0, Key({Int64(i)}),
                                {kColumnID}, &values),
                googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Read
  GOOGLESQL_EXPECT_OK(
      storage_.Read(lookup_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, DeleteUsingPrefixKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time lookup_ts = delete_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0,
                            KeyRange::Prefix(Key({String("key")}))));
  // Lookup
  for (int i = 0; i < 5; i++) {
    std::vector<googlesql::Value> values;
    EXPECT_THAT(
        storage_.Lookup(lookup_ts, kTableId0, Key({String("key"), Int64(i)}),
                        {kColumnID}, &values),
        googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  }
  // Read
  GOOGLESQL_EXPECT_OK(storage_.Read(lookup_ts, kTableId0,
                          KeyRange::ClosedOpen(Key({String("key"), Int64(0)}),
                                               Key({String("key"), Int64(5)})),
                          {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, LookupShouldReturnMostRecentColumnValues) {
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time write_after_delete_ts = delete_ts + absl::Seconds(1);
  absl::Time lookup_after_second_write_ts =
      write_after_delete_ts + absl::Seconds(1);
  Key key({Int64(1)});

  // Write key with column value = true.
  GOOGLESQL_EXPECT_OK(
      storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  // Delete key.
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
  // Write key without column value.
  GOOGLESQL_EXPECT_OK(storage_.Write(write_after_delete_ts, kTableId0, key, {}, {}));
  // Lookup should return empty column value.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(lookup_after_second_write_ts, kTableId0, key,
                            {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
  // Read should return empty column values.
  GOOGLESQL_EXPECT_OK(storage_.Read(lookup_after_second_write_ts, kTableId0,
                          kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_FALSE(itr_->ColumnValue(0).is_valid());
  EXPECT_EQ(itr_->Key(), Key({Int64(1)}));
}

TEST_F(InMemoryStorageTest, LookupAtOrAfterDeleteTimestampReturnsInvalidValue) {
  Key key({Int64(1)});
  absl::Time write_ts = absl::Now();
  absl::Time delete_ts = write_ts + absl::Seconds(1);
  absl::Time after_delete_ts = delete_ts + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, key, {kColumnID},
                           {String("value-10")}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));

  // Lookup at delete timestamp.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(storage_.Lookup(delete_ts, kTableId0, key, {kColumnID}, &values),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // Lookup after delete timestamp.
  values.clear();
  EXPECT_THAT(
      storage_.Lookup(after_delete_ts, kTableId0, key, {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, LookupBeforeDeleteTimestampReturnsValidValue) {
  absl::Time write_ts = absl::Now();
  absl::Time before_delete_ts = write_ts + absl::Seconds(1);
  absl::Time delete_ts = before_delete_ts + absl::Seconds(1);
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, key, {kColumnID},
                           {String("value-10")}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(delete_ts, kTableId0, KeyRange::Point(key)));
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(before_delete_ts, kTableId0, key, {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-10")));
}

TEST_F(InMemoryStorageTest, SnapshotRead) {
  absl::Time write_ts = absl::Now();
  absl::Time snapshot_read_ts = write_ts + absl::Seconds(1);
  absl::Time second_write_ts = snapshot_read_ts + absl::Seconds(1);
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, key, {kColumnID},
                           {String("value-10")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(second_write_ts, kTableId0, key, {kColumnID},
                           {String("value-20")}));

  // Snapshot Read.
  GOOGLESQL_EXPECT_OK(storage_.Read(snapshot_read_ts, kTableId0, kKeyRange0To5,
                          {kColumnID}, &itr_));
  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->ColumnValue(0), String("value-10"));
}

TEST_F(InMemoryStorageTest, ReadUsingKeyRangeAll) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  // Read using KeyRange::All()
  GOOGLESQL_EXPECT_OK(
      storage_.Read(read_ts, kTableId0, KeyRange::All(), {kColumnID}, &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadUsingPointKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  GOOGLESQL_EXPECT_OK(storage_.Read(read_ts, kTableId0, KeyRange::Point(Key({Int64(0)})),
                          {kColumnID}, &itr_));

  EXPECT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->NumColumns(), 1);
  EXPECT_EQ(itr_->Key(), Key({Int64(0)}));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, ReadUsingPrefixKeyRange) {
  absl::Time write_ts = absl::Now();
  absl::Time read_ts = write_ts + absl::Seconds(1);

  for (int i = 0; i < 5; i++) {
    Key key({String("key"), Int64(i)});
    GOOGLESQL_EXPECT_OK(
        storage_.Write(write_ts, kTableId0, key, {kColumnID}, {Bool(true)}));
  }

  // Read
  GOOGLESQL_EXPECT_OK(storage_.Read(read_ts, kTableId0,
                          KeyRange::Prefix(Key({String("key")})), {kColumnID},
                          &itr_));
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(itr_->Next());
    EXPECT_EQ(itr_->NumColumns(), 1);
    EXPECT_EQ(itr_->Key(), Key({String("key"), Int64(i)}));
  }
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest,
       ReadUsingInvalidKeyRangeEndpointsReturnsInternalError) {
  absl::Time t0 = absl::Now();
  Key key({Int64(1)});

  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, key, {}, {}));

  EXPECT_THAT(
      storage_.Read(t0, kTableId0, KeyRange::OpenClosed(Key({Int64(0)}), key),
                    {}, &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Read(t0, kTableId0,
                    KeyRange::OpenOpen(Key({Int64(0)}), Key({Int64(2)})), {},
                    &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
  EXPECT_THAT(
      storage_.Read(t0, kTableId0, KeyRange::ClosedClosed(Key({Int64(0)}), key),
                    {}, &itr_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(InMemoryStorageTest, DroppedTablesAreRemovedAfterRetentionPeriod) {
  absl::Time t0 = absl::Now();

  // Write into 2 tables.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId1, Key({Int64(10)}), {kColumnID},
                           {String("value-10")}));

  // Drop the first table.
  storage_.MarkDroppedTable(t0, kTableId0);

  // Expire the table.
  storage_.CleanUpDeletedTables(t0 + absl::Hours(1) + absl::Seconds(1));

  // Lookup should return not found for first table.
  std::vector<googlesql::Value> values;
  EXPECT_THAT(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId1, Key({Int64(10)}), {kColumnID}, &values));
}

TEST_F(InMemoryStorageTest, DroppedColumnsAreRemovedAfterRetentionPeriod) {
  absl::Time t0 = absl::Now();

  // Write multiple rows to column.
  for (int i = 0; i < 10; i++) {
    GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(i)}), {kColumnID},
                             {String("value-1")}));
  }

  // Drop and expire the column.
  storage_.MarkDroppedColumn(t0, kTableId0, kColumnID);
  storage_.CleanUpDeletedColumns(t0 + absl::Hours(1) + absl::Seconds(1));

  // Lookup of column should return empty values in all rows.
  for (int i = 0; i < 10; i++) {
    std::vector<googlesql::Value> values;
    GOOGLESQL_EXPECT_OK(
        storage_.Lookup(t0, kTableId0, Key({Int64(i)}), {kColumnID}, &values));
    EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
  }
}

TEST_F(InMemoryStorageTest, ExpiredCellsThatCoverRetentionPeriodAreKept) {
  absl::Time t0 = absl::Now();
  absl::Time t1 = t0 + absl::Minutes(10);
  absl::Time t2 = t0 + absl::Minutes(40);
  absl::Time t3 = t0 + absl::Hours(1) + absl::Seconds(1);
  absl::Time t4 = t2 + absl::Hours(1) + absl::Seconds(1);

  // Write into column.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-0")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t2, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-2")}));

  // Write to remove expired values.
  GOOGLESQL_EXPECT_OK(storage_.Write(t3, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-3")}));

  // Lookup of t1 should return the first value which shouldn't be cleaned up
  // because it covers the retention period.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t1, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(String("value-0")));

  GOOGLESQL_EXPECT_OK(storage_.Write(t4, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-4")}));

  // Lookup of t1 should return an empty value because the first value was
  // cleaned up as it doesn't cover the retention period.
  values.clear();
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t1, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
}

TEST_F(InMemoryStorageTest, RemoveExpiredVersionsFromCellOnWrite) {
  absl::Time t0 = absl::Now();
  absl::Time t1 = t0 + absl::Seconds(1);
  absl::Time t2 = t1 + absl::Hours(1);

  // Write into column.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-0")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t1, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  // Write should remove the t0 value as that will be expired.
  GOOGLESQL_EXPECT_OK(storage_.Write(t2, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-2")}));

  // Lookup of t0 should return an empty value because the value was cleaned up.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
}

TEST_F(InMemoryStorageTest, RemoveExpiredVersionsFromCellOnDelete) {
  absl::Time t0 = absl::Now();
  absl::Time t1 = t0 + absl::Seconds(1);
  absl::Time t2 = t1 + absl::Hours(1);

  // Write into column.
  GOOGLESQL_EXPECT_OK(storage_.Write(t0, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-0")}));
  GOOGLESQL_EXPECT_OK(storage_.Write(t1, kTableId0, Key({Int64(1)}), {kColumnID},
                           {String("value-1")}));

  // Delete should remove the t0 value as that will be expired.
  GOOGLESQL_EXPECT_OK(storage_.Delete(t2, kTableId0, KeyRange::Point(Key({Int64(1)}))));

  // Lookup of t0 should return an empty value because the value was cleaned up.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(t0, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(googlesql::Value()));
}

// Tests for PurgeExpiredDeletedRows.
//
// Delete() only appends a tombstone, so a deleted row keeps its key and cells
// for the lifetime of the process. PurgeExpiredDeletedRows erases rows whose
// deletion marker is older than the version retention period, i.e. rows that
// are invisible at every timestamp a transaction may legally read.
//
// The retention period is set short so the tests do not have to sleep; the
// timestamps are constructed relative to now.

TEST_F(InMemoryStorageTest, PurgeRemovesRowDeletedBeforeRetentionWindow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();
  absl::Time write_ts = now - absl::Minutes(10);
  absl::Time delete_ts = now - absl::Minutes(5);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {Bool(true)}));
  GOOGLESQL_EXPECT_OK(
      storage_.Delete(delete_ts, kTableId0, KeyRange::Point(Key({Int64(1)}))));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 1);

  // The row is gone entirely, so a read at any timestamp finds nothing.
  GOOGLESQL_EXPECT_OK(storage_.Read(now, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());
}

TEST_F(InMemoryStorageTest, PurgeKeepsLiveRow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 0);

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(now, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

// The safety property that matters: a row deleted inside the retention window
// is still readable at an earlier timestamp, so it must not be erased.
TEST_F(InMemoryStorageTest, PurgeKeepsRowDeletedInsideRetentionWindow) {
  storage_.SetVersionRetentionPeriod(absl::Hours(1));
  absl::Time now = absl::Now();
  absl::Time write_ts = now - absl::Minutes(30);
  absl::Time delete_ts = now - absl::Minutes(10);

  GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, kTableId0, Key({Int64(1)}), {kColumnID},
                           {Bool(true)}));
  GOOGLESQL_EXPECT_OK(
      storage_.Delete(delete_ts, kTableId0, KeyRange::Point(Key({Int64(1)}))));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 0);

  // A stale read between the write and the delete still sees the row.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(now - absl::Minutes(20), kTableId0, Key({Int64(1)}),
                            {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

// A row deleted and then re-inserted is live and must survive.
TEST_F(InMemoryStorageTest, PurgeKeepsResurrectedRow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(8), kTableId0,
                            KeyRange::Point(Key({Int64(1)}))));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(6), kTableId0, Key({Int64(1)}),
                           {kColumnID}, {Bool(false)}));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 0);

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(now, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(false)));
}

TEST_F(InMemoryStorageTest, PurgeScopedToNamedTablesOnly) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();
  absl::Time write_ts = now - absl::Minutes(10);
  absl::Time delete_ts = now - absl::Minutes(5);

  for (const TableID& table_id : {kTableId0, kTableId1}) {
    GOOGLESQL_EXPECT_OK(storage_.Write(write_ts, table_id, Key({Int64(1)}), {kColumnID},
                             {Bool(true)}));
    GOOGLESQL_EXPECT_OK(
        storage_.Delete(delete_ts, table_id, KeyRange::Point(Key({Int64(1)}))));
  }

  // Only table 0 is swept.
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {kTableId0}), 1);
  GOOGLESQL_EXPECT_OK(storage_.Read(now, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  EXPECT_FALSE(itr_->Next());

  // Table 1 still holds its tombstoned row, and a later sweep reclaims it.
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {kTableId1}), 1);
}

TEST_F(InMemoryStorageTest, PurgeIsIdempotent) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(5), kTableId0,
                            KeyRange::Point(Key({Int64(1)}))));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 1);
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 0);
}

// PurgeExpiredVersions covers the other half of the waste: a LIVE row that was
// updated a few times and then left alone. RemoveExpiredVersions() only runs
// while its own cell is being written, so those superseded versions are never
// reclaimed on their own.
TEST_F(InMemoryStorageTest, PurgeVersionsTrimsSupersededVersionsOfLiveRow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  // Five writes to one cell, all well outside the retention window.
  for (int i = 10; i >= 6; --i) {
    GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(i), kTableId0,
                             Key({Int64(1)}), {kColumnID}, {Int64(i)}));
  }

  // Four of the five versions are erased; the newest before the retention
  // floor is kept so reads inside the window still resolve.
  EXPECT_EQ(storage_.PurgeExpiredVersions(now, {}), 4);

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(now, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(6)));
}

TEST_F(InMemoryStorageTest, PurgeVersionsKeepsVersionsInsideRetentionWindow) {
  storage_.SetVersionRetentionPeriod(absl::Hours(1));
  absl::Time now = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(30), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Int64(1)}));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Int64(2)}));

  // Both versions are readable inside the window, so neither may be erased.
  EXPECT_EQ(storage_.PurgeExpiredVersions(now, {}), 0);

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(now - absl::Minutes(25), kTableId0, Key({Int64(1)}),
                            {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Int64(1)));
}

TEST_F(InMemoryStorageTest, PurgeVersionsIsIdempotent) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  for (int i = 10; i >= 8; --i) {
    GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(i), kTableId0,
                             Key({Int64(1)}), {kColumnID}, {Int64(i)}));
  }

  EXPECT_EQ(storage_.PurgeExpiredVersions(now, {}), 2);
  EXPECT_EQ(storage_.PurgeExpiredVersions(now, {}), 0);
}

TEST_F(InMemoryStorageTest, PurgeOnUnknownTableIsNoOp) {
  absl::Time now = absl::Now();
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {"no_such_table"}), 0);
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 0);
  EXPECT_EQ(storage_.PurgeExpiredVersions(now, {"no_such_table"}), 0);
}

TEST_F(InMemoryStorageTest, PurgeRemovesOnlyExpiredRowsFromMixedTable) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  // Row 1: deleted long ago -> purgeable.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(5), kTableId0,
                            KeyRange::Point(Key({Int64(1)}))));
  // Row 2: still live -> must survive.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(2)}), {kColumnID}, {Bool(true)}));

  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 1);

  GOOGLESQL_EXPECT_OK(storage_.Read(now, kTableId0, kKeyRange0To5, {kColumnID}, &itr_));
  ASSERT_TRUE(itr_->Next());
  EXPECT_EQ(itr_->Key(), Key({Int64(2)}));
  EXPECT_FALSE(itr_->Next());
}


// Tests for the PurgeWindow bounds.
//
// A harness that seeds a database before a run needs the seed data to survive
// reclamation. Seed data is the OLDEST data present, so an unbounded sweep
// reaches it first -- these cover the bounds that prevent that.

TEST_F(InMemoryStorageTest, PurgeKeepsSeedRowDeletedInsideWindow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();
  absl::Time seed_done = now - absl::Minutes(30);

  // Seeded before the run, then deleted by a test during it. Without the seed
  // guard the tombstone alone would make this row purgeable.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(40), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(10), kTableId0,
                            KeyRange::Point(Key({Int64(1)}))));

  PurgeWindow window;
  window.not_before = seed_done;
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}, window), 0);

  // Without the bound the same row is purgeable, proving the guard did it.
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}), 1);
}

TEST_F(InMemoryStorageTest, PurgeRemovesRowCreatedAfterNotBefore) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();
  absl::Time seed_done = now - absl::Minutes(30);

  // Written and deleted entirely inside the window: this is run churn.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(2)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(10), kTableId0,
                            KeyRange::Point(Key({Int64(2)}))));

  PurgeWindow window;
  window.not_before = seed_done;
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}, window), 1);
}

TEST_F(InMemoryStorageTest, PurgeKeepsRowDeletedInsideNotAfterLag) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  // Deleted 30s ago; a one-minute lag must hold it back even though the
  // retention period alone would allow the sweep.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(3)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Seconds(30), kTableId0,
                            KeyRange::Point(Key({Int64(3)}))));

  PurgeWindow window;
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}, window), 0);
}

TEST_F(InMemoryStorageTest, PurgeVersionsKeepsSeedVersionOfLiveRow) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();
  absl::Time seed_done = now - absl::Minutes(30);

  // One seed version plus two written during the run. The seed version must
  // survive; the superseded in-window version is reclaimable.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(40), kTableId0,
                           Key({Int64(4)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(4)}), {kColumnID}, {Bool(false)}));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(4)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = seed_done;
  storage_.PurgeExpiredVersions(now, {}, window);

  // The seed version is still readable at a timestamp inside the seed era.
  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(storage_.Lookup(now - absl::Minutes(35), kTableId0,
                            Key({Int64(4)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

TEST_F(InMemoryStorageTest, PurgeWindowLeavesRetentionBehaviourUnchanged) {
  storage_.SetVersionRetentionPeriod(absl::Seconds(1));
  absl::Time now = absl::Now();

  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(5)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Delete(now - absl::Minutes(8), kTableId0,
                            KeyRange::Point(Key({Int64(5)}))));

  // An empty window is the documented default: same result as no window.
  EXPECT_EQ(storage_.PurgeExpiredDeletedRows(now, {}, PurgeWindow{}), 1);
}


// Tests for DeleteRowsInWindow -- the opt-in destructive sweep.
//
// This erases LIVE rows, so its bounds are the only thing standing between a
// harness and its seed data. Each bound is covered on both sides.

TEST_F(InMemoryStorageTest, DeleteRowsInWindowKeepsSeedRow) {
  absl::Time now = absl::Now();
  absl::Time seed_done = now - absl::Minutes(30);

  // Committed before seeding finished: must survive.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(40), kTableId0,
                           Key({Int64(1)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = seed_done;
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 0);

  std::vector<googlesql::Value> values;
  GOOGLESQL_EXPECT_OK(
      storage_.Lookup(now, kTableId0, Key({Int64(1)}), {kColumnID}, &values));
  EXPECT_THAT(values, testing::ElementsAre(Bool(true)));
}

TEST_F(InMemoryStorageTest, DeleteRowsInWindowRemovesTestRow) {
  absl::Time now = absl::Now();

  // Committed inside the window: this is test churn and is erased even though
  // it is live and was never deleted by the application.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(2)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = now - absl::Minutes(30);
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 1);

  std::vector<googlesql::Value> values;
  EXPECT_THAT(
      storage_.Lookup(now, kTableId0, Key({Int64(2)}), {kColumnID}, &values),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(InMemoryStorageTest, DeleteRowsInWindowKeepsRowInsideNotAfterLag) {
  absl::Time now = absl::Now();

  // Committed 30s ago, inside the one-minute hold-back: a running test may
  // still be reading it.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Seconds(30), kTableId0,
                           Key({Int64(3)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = now - absl::Minutes(30);
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 0);
}

TEST_F(InMemoryStorageTest, DeleteRowsInWindowUsesCommitTimestampOfInsert) {
  absl::Time now = absl::Now();

  // Seeded before the boundary, then UPDATED inside the window. The row is
  // still seed data: the insert's commit timestamp decides, not the update's.
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(40), kTableId0,
                           Key({Int64(4)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(10), kTableId0,
                           Key({Int64(4)}), {kColumnID}, {Bool(false)}));

  PurgeWindow window;
  window.not_before = now - absl::Minutes(30);
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 0);
}

TEST_F(InMemoryStorageTest, DeleteRowsInWindowScopedToNamedTablesOnly) {
  absl::Time now = absl::Now();
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(5)}), {kColumnID}, {Bool(true)}));
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId1,
                           Key({Int64(5)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = now - absl::Minutes(30);
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {kTableId0}, window), 1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {kTableId1}, window), 1);
}

TEST_F(InMemoryStorageTest, DeleteRowsInWindowIsIdempotent) {
  absl::Time now = absl::Now();
  GOOGLESQL_EXPECT_OK(storage_.Write(now - absl::Minutes(20), kTableId0,
                           Key({Int64(6)}), {kColumnID}, {Bool(true)}));

  PurgeWindow window;
  window.not_before = now - absl::Minutes(30);
  window.not_after = now - absl::Minutes(1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 1);
  EXPECT_EQ(storage_.DeleteRowsInWindow(now, {}, window), 0);
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
