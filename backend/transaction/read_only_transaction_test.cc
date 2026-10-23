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

#include "backend/transaction/read_only_transaction.h"

#include <ctime>

#include "googlesql/public/type.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

class ReadOnlyTransactionTest : public testing::Test {
 protected:
  TransactionID txn_id_ = 1;
  Clock clock_;
  absl::Time t0_ = clock_.Now();
  InMemoryStorage storage_;
  LockManager lock_manager_ = LockManager(&clock_);
  VersionedCatalog versioned_catalog_;
  absl::Time read_timestamp_;
};

TEST_F(ReadOnlyTransactionTest, StongSnapshotReadTimestamp) {
  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kStrongRead;

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &versioned_catalog_);
  read_timestamp_ = txn.read_timestamp();
  EXPECT_GT(clock_.Now(), read_timestamp_);
}

TEST_F(ReadOnlyTransactionTest, ExactStalenessSnapshotReadTimestamp) {
  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kExactStaleness;
  opts.staleness = absl::Microseconds(1);

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &versioned_catalog_);
  read_timestamp_ = txn.read_timestamp();
  EXPECT_GT(clock_.Now() - absl::Microseconds(1), read_timestamp_);
}

TEST_F(ReadOnlyTransactionTest, MaxStalenessSnapshotReadTimestamp) {
  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kMaxStaleness;
  opts.staleness = absl::Microseconds(1);

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &versioned_catalog_);
  read_timestamp_ = txn.read_timestamp();
  EXPECT_GT(clock_.Now(), read_timestamp_);
  EXPECT_LE(opts.timestamp - opts.staleness, read_timestamp_);
}

TEST_F(ReadOnlyTransactionTest, MinTimestampSnapshotReadTimestamp) {
  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kMinTimestamp;
  opts.timestamp = t0_;

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &versioned_catalog_);
  read_timestamp_ = txn.read_timestamp();
  EXPECT_GT(clock_.Now(), read_timestamp_);
  EXPECT_LE(t0_, read_timestamp_);
}

TEST_F(ReadOnlyTransactionTest, ExactTimestampSnapshotReadTimestamp) {
  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kExactTimestamp;
  opts.timestamp = t0_;

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &versioned_catalog_);
  read_timestamp_ = txn.read_timestamp();
  EXPECT_EQ(t0_, read_timestamp_);
}

TEST_F(ReadOnlyTransactionTest, GetSchema) {
  VersionedCatalog catalog;
  googlesql::TypeFactory type_factory{};
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t0_, test::CreateSchemaWithOneTable(&type_factory)));

  // A strong read finds the schema created at t0_.
  ReadOnlyOptions opts1;
  opts1.bound = TimestampBound::kStrongRead;

  ReadOnlyTransaction txn1(opts1, txn_id_, &clock_, &storage_, &lock_manager_,
                           &catalog);
  ASSERT_NE(txn1.schema(), nullptr);
  ASSERT_NE(txn1.schema()->FindTable("test_table"), nullptr);

  // A snapshot read at t0_ - 1 us gets an empty schema.
  ReadOnlyOptions opts2;
  opts2.bound = TimestampBound::kExactTimestamp;
  opts2.timestamp = t0_ - absl::Microseconds(1);

  ReadOnlyTransaction txn2(opts2, txn_id_, &clock_, &storage_, &lock_manager_,
                           &catalog);
  ASSERT_NE(txn2.schema(), nullptr);
  EXPECT_EQ(txn2.schema()->FindTable("test_table"), nullptr);
}

// Once a schema version has been pruned, a read at a timestamp it
// covered is refused rather than served with a newer schema.
TEST_F(ReadOnlyTransactionTest, ReadBelowPrunedSchemaVersionIsRefused) {
  VersionedCatalog catalog;
  googlesql::TypeFactory type_factory{};
  const absl::Time t1 = t0_ + absl::Minutes(1);
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t0_, test::CreateSchemaWithOneTable(&type_factory)));
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t1, test::CreateSchemaWithOneTable(&type_factory)));

  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kExactTimestamp;
  opts.timestamp = t0_ + absl::Seconds(30);  // covered by the t0_ version

  ReadArg read_arg;
  read_arg.table = "test_table";
  read_arg.key_set = KeySet::All();
  read_arg.columns = {"int64_col"};

  // Served before the prune.
  {
    ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                            &catalog);
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_EXPECT_OK(txn.Read(read_arg, &cursor));
  }

  EXPECT_EQ(catalog.PruneSchemasOlderThan(absl::InfiniteFuture()), 1);
  EXPECT_EQ(catalog.MinReadableSchemaTime(), t1);

  // Refused after it, with the retention error; a read at t1 still works.
  {
    ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                            &catalog);
    std::unique_ptr<RowCursor> cursor;
    EXPECT_THAT(txn.Read(read_arg, &cursor),
                googlesql_base::testing::StatusIs(
                    absl::StatusCode::kFailedPrecondition));
  }
  {
    ReadOnlyOptions at_t1;
    at_t1.bound = TimestampBound::kExactTimestamp;
    at_t1.timestamp = t1;
    ReadOnlyTransaction txn(at_t1, txn_id_, &clock_, &storage_, &lock_manager_,
                            &catalog);
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_EXPECT_OK(txn.Read(read_arg, &cursor));
  }
}

// schema() pins the version it resolved, so pruning it from the
// catalog does not free it while the transaction is using it.
TEST_F(ReadOnlyTransactionTest, PinnedSchemaSurvivesPrune) {
  VersionedCatalog catalog;
  googlesql::TypeFactory type_factory{};
  const absl::Time t1 = t0_ + absl::Minutes(1);
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t0_, test::CreateSchemaWithOneTable(&type_factory)));
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t1, test::CreateSchemaWithOneTable(&type_factory)));

  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kExactTimestamp;
  opts.timestamp = t0_ + absl::Seconds(30);
  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &catalog);
  const Schema* pinned = txn.schema();
  ASSERT_NE(pinned, nullptr);

  EXPECT_EQ(catalog.PruneSchemasOlderThan(absl::InfiniteFuture()), 1);
  EXPECT_NE(catalog.GetSchema(opts.timestamp), pinned);  // catalog moved on

  EXPECT_EQ(txn.schema(), pinned);
  EXPECT_NE(pinned->FindTable("test_table"), nullptr);
}

TEST_F(ReadOnlyTransactionTest, WaitsForFutureReadTime) {
  VersionedCatalog catalog;
  googlesql::TypeFactory type_factory{};
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t0_, test::CreateSchemaWithOneTable(&type_factory)));

  ReadOnlyOptions opts;
  opts.bound = TimestampBound::kExactTimestamp;
  opts.timestamp = clock_.Now() + absl::Microseconds(1000);

  ReadOnlyTransaction txn(opts, txn_id_, &clock_, &storage_, &lock_manager_,
                          &catalog);
  ASSERT_NE(txn.schema(), nullptr);
  ASSERT_NE(txn.schema()->FindTable("test_table"), nullptr);

  // A snapshot read at t0_ + 1000 will wait for read time to become current
  // before being able to access database and return the schema.
  EXPECT_GE(clock_.Now(), opts.timestamp);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
