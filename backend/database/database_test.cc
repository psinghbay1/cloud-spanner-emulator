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

#include "backend/database/database.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "common/config.h"
#include "common/errors.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql_base::testing::StatusIs;

constexpr char kDatabaseId[] = "test-db";

// Sets --schema_version_retention for one test and restores it afterwards.
class ScopedSchemaVersionRetention {
 public:
  explicit ScopedSchemaVersionRetention(absl::Duration retention)
      : previous_(config::schema_version_retention()) {
    config::set_schema_version_retention(retention);
  }
  ~ScopedSchemaVersionRetention() {
    config::set_schema_version_retention(previous_);
  }

 private:
  const absl::Duration previous_;
};

class DatabaseTest : public ::testing::Test {
 public:
  DatabaseTest() = default;

  ReadArg read_column(std::string table_name, std::string column_name) {
    ReadArg args;
    args.table = table_name;
    args.key_set = KeySet::All();
    args.columns = std::vector<std::string>{column_name};
    return args;
  }

  // Creates table T, then adds c1 and c2 in two DDL batches. Returns a
  // timestamp between the two batches, covered by the c1-only schema.
  absl::Time CreateDatabaseWithTwoDdlBatches(std::unique_ptr<Database>* db) {
    std::vector<std::string> create_statements = {R"(
      CREATE TABLE T(
        k1 INT64,
        k2 INT64,
      ) PRIMARY KEY(k1)
    )"};
    auto created = Database::Create(
        &clock_, kDatabaseId,
        SchemaChangeOperation{.statements = create_statements});
    EXPECT_TRUE(created.ok()) << created.status();
    *db = std::move(*created);
    AddColumn(db->get(), "c1");
    const absl::Time between_ddls = clock_.Now();
    AddColumn(db->get(), "c2");
    return between_ddls;
  }

  void AddColumn(Database* db, absl::string_view column) {
    absl::Status backfill_status;
    int completed_statements;
    absl::Time commit_ts;
    GOOGLESQL_EXPECT_OK(db->UpdateSchema(
        SchemaChangeOperation{.statements = {absl::StrCat(
                                  "ALTER TABLE T ADD COLUMN ", column,
                                  " INT64")}},
        &completed_statements, &commit_ts, &backfill_status));
    GOOGLESQL_EXPECT_OK(backfill_status);
  }

  static ReadOnlyOptions ExactTimestamp(absl::Time timestamp) {
    ReadOnlyOptions options;
    options.bound = TimestampBound::kExactTimestamp;
    options.timestamp = timestamp;
    return options;
  }

 protected:
  Clock clock_;
};

TEST_F(DatabaseTest, CreateSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(&clock_, kDatabaseId, SchemaChangeOperation{}));
  // Verifies that by default, a GoogleSQL database is created.
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));
  // Verifies that by default, a GoogleSQL database is created.
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseTest, CreateWithGSQLDialectSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .database_dialect =
                  database_api::DatabaseDialect::GOOGLE_STANDARD_SQL}));
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .statements = create_statements,
              .database_dialect =
                  database_api::DatabaseDialect::GOOGLE_STANDARD_SQL}));
  EXPECT_EQ(database->dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseTest, CreateWithPostgresDialectSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}));
  EXPECT_EQ(database->dialect(), database_api::DatabaseDialect::POSTGRESQL);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 bigint primary key,
      k2 bigint
    )
  )",
                                                R"(
    CREATE INDEX I on T(k1))"};

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .statements = create_statements,
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}));
  EXPECT_EQ(database->dialect(), database_api::DatabaseDialect::POSTGRESQL);
}

TEST_F(DatabaseTest, UpdateSchemaSuccessful) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db, Database::Create(&clock_, kDatabaseId, SchemaChangeOperation{}));

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )",
                                                R"(
    CREATE INDEX I on T(k1)
  )"};

  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  GOOGLESQL_EXPECT_OK(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status));
  GOOGLESQL_EXPECT_OK(backfill_status);
}

TEST_F(DatabaseTest, UpdateSchemaPartialSuccess) {
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));

  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(1), Int64(2)}});
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(2), Int64(2)}});
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k1", "k2"},
               {{Int64(3), Int64(2)}});
  GOOGLESQL_ASSERT_OK(txn->Write(m));
  GOOGLESQL_ASSERT_OK(txn->Commit());

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T1(
      a INT64,
    ) PRIMARY KEY(a)
  )",
                                                R"(
    CREATE UNIQUE INDEX Idx on T(k2)
  )",
                                                R"(
    CREATE TABLE T2(
      b INT64,
    ) PRIMARY KEY(b)
  )"};

  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;

  // The statements are semantically valid, indicated by an OK return status.
  GOOGLESQL_EXPECT_OK(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status));

  // But the backfill statements fail.
  EXPECT_EQ(backfill_status,
            error::UniqueIndexViolationOnIndexCreation("Idx", "{Int64(2)}"));

  // Only the first statement in the batch is successfuly applied.
  EXPECT_EQ(completed_statements, 1);
}

TEST_F(DatabaseTest, ConcurrentSchemaChangeIsAborted) {
  auto current_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(0);

  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  // Initiate a Read inside a read-write transaction to acquire locks.
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "k1"), &row_cursor));

  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  EXPECT_EQ(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status),
      error::ConcurrentSchemaChangeOrReadWriteTxnInProgress());

  config::set_abort_current_transaction_probability(current_probability);
}

// Without --schema_version_retention every schema version stays readable for
// the version retention period, as before the flag existed.
TEST_F(DatabaseTest, DefaultSchemaVersionRetentionKeepsOldVersionsReadable) {
  ScopedSchemaVersionRetention retention(absl::InfiniteDuration());
  std::unique_ptr<Database> db;
  const absl::Time between_ddls = CreateDatabaseWithTwoDdlBatches(&db);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> txn,
      db->CreateReadOnlyTransaction(ExactTimestamp(between_ddls)));
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "c1"), &row_cursor));
  EXPECT_EQ(txn->schema()->FindTable("T")->FindColumn("c2"), nullptr);
}

// With --schema_version_retention=0s each DDL batch drops the versions it
// supersedes, and a read at a timestamp they covered is refused with the
// retention error rather than served with a newer schema.
TEST_F(DatabaseTest, ZeroSchemaVersionRetentionRefusesReadsAtPrunedVersions) {
  ScopedSchemaVersionRetention retention(absl::ZeroDuration());
  std::unique_ptr<Database> db;
  const absl::Time between_ddls = CreateDatabaseWithTwoDdlBatches(&db);

  EXPECT_THAT(db->CreateReadOnlyTransaction(ExactTimestamp(between_ddls)),
              StatusIs(absl::StatusCode::kFailedPrecondition));

  ReadOnlyOptions strong;
  strong.bound = TimestampBound::kStrongRead;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ReadOnlyTransaction> txn,
                       db->CreateReadOnlyTransaction(strong));
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "c2"), &row_cursor));
}

// A snapshot that resolved its schema before a prune keeps that version
// pinned, so retention cannot free a schema still in use.
TEST_F(DatabaseTest, InFlightSnapshotKeepsSchemaAcrossRetentionPrune) {
  ScopedSchemaVersionRetention retention(absl::ZeroDuration());
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));
  AddColumn(db.get(), "c1");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> txn,
      db->CreateReadOnlyTransaction(ExactTimestamp(clock_.Now())));
  const Schema* pinned = txn->schema();
  ASSERT_NE(pinned->FindTable("T")->FindColumn("c1"), nullptr);

  AddColumn(db.get(), "c2");

  EXPECT_EQ(txn->schema(), pinned);
  EXPECT_EQ(pinned->FindTable("T")->FindColumn("c2"), nullptr);
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "c1"), &row_cursor));
}

// A read-write transaction created before several DDL batches under
// --schema_version_retention=0s initialises on the latest schema.
TEST_F(DatabaseTest, ReadWriteTransactionUsesLatestSchemaAfterRetentionPrune) {
  ScopedSchemaVersionRetention retention(absl::ZeroDuration());
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  AddColumn(db.get(), "c1");
  AddColumn(db.get(), "c2");

  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "c2"), &row_cursor));
  GOOGLESQL_EXPECT_OK(txn->Rollback());
}

TEST_F(DatabaseTest, SchemaChangeLocksSuccesfullyReleased) {
  std::vector<std::string> create_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = create_statements}));

  // Schema update will fail.
  std::vector<std::string> update_statements = {R"(
    CREATE TABLE T(
      k1 INT64,
      k2 INT64,
    ) PRIMARY KEY(k1)
  )"};
  absl::Status backfill_status;
  int completed_statements;
  absl::Time commit_ts;
  EXPECT_FALSE(
      db->UpdateSchema(SchemaChangeOperation{.statements = update_statements},
                       &completed_statements, &commit_ts, &backfill_status)
          .ok());

  // Can still run transactions as locks would have been released.
  std::unique_ptr<RowCursor> row_cursor;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadWriteTransaction> txn,
      db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState()));
  GOOGLESQL_EXPECT_OK(txn->Read(read_column("T", "k1"), &row_cursor));
  GOOGLESQL_EXPECT_OK(txn->Commit());
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
