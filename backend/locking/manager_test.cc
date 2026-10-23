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

#include "backend/locking/manager.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/request.h"
#include "common/config.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using googlesql::values::Int64;

class LockManagerTest : public testing::Test {
 public:
  LockManagerTest()
      : request_(LockMode::kExclusive, "table", KeyRange::All(), {}) {}

  Clock* clock() { return &clock_; }
  LockManager* manager() { return &manager_; }
  const LockRequest& request() { return request_; }

  // A key made of one int64 column, so ranges are easy to reason about.
  static Key K(int64_t v) { return Key({Int64(v)}); }

  // Exclusive lock on [start, limit) of `table`.
  static LockRequest Write(const std::string& table, int64_t start,
                           int64_t limit) {
    return LockRequest(LockMode::kExclusive, table,
                       KeyRange::ClosedOpen(K(start), K(limit)), {});
  }

  // Shared lock on [start, limit) of `table`.
  static LockRequest Read(const std::string& table, int64_t start,
                          int64_t limit) {
    return LockRequest(LockMode::kShared, table,
                       KeyRange::ClosedOpen(K(start), K(limit)), {});
  }

  // Renders a handle's outcome for a failure message. A bare "Actual: true"
  // from EXPECT_FALSE(IsAborted()) says nothing about which side lost or why.
  static std::string Describe(const std::string& name, LockHandle* handle) {
    std::string out = absl::StrCat(name, "(tid=", handle->tid(), "): ");
    if (handle->IsAborted()) {
      absl::StrAppend(&out, "ABORTED status=\"", handle->Wait().message(), "\"");
    } else if (handle->IsBlocked()) {
      absl::StrAppend(&out, "BLOCKED");
    } else {
      absl::StrAppend(&out, "granted");
    }
    return out;
  }

  // The full picture for a two-transaction scenario: what each side asked for
  // and what happened to it.
  static std::string Scenario(const std::string& what,
                              const std::string& request_1, LockHandle* lh1,
                              const std::string& request_2, LockHandle* lh2) {
    return absl::StrCat("\n  scenario : ", what,
                        "\n  txn 1    : ", request_1,
                        "\n           : ", Describe("txn1", lh1),
                        "\n  txn 2    : ", request_2,
                        "\n           : ", Describe("txn2", lh2),
                        "\n  expected : see the comment above this assertion");
  }

  // A handle the manager CAN wound: its abort callback succeeds. Handle() uses a
  // null callback, which makes TryAbortTransaction fail and the requester lose.
  std::unique_ptr<LockHandle> AbortableHandle(int64_t id) {
    return manager()->CreateHandle(
        TransactionID(id), []() { return absl::OkStatus(); },
        TransactionPriority(1));
  }

  // An abortable handle with an explicit priority (begin time; lower = older).
  std::unique_ptr<LockHandle> AbortableHandle(int64_t id, int64_t priority) {
    return manager()->CreateHandle(
        TransactionID(id), []() { return absl::OkStatus(); },
        TransactionPriority(priority));
  }

  std::unique_ptr<LockHandle> Handle(int64_t id) {
    return manager()->CreateHandle(TransactionID(id), /*try_abort_fn=*/nullptr,
                                   TransactionPriority(1));
  }

 private:
  Clock clock_;
  LockManager manager_ = LockManager(&clock_);
  LockRequest request_;
};

// The tests below exercise --enable_key_range_locking, which is off by default.
class KeyRangeLockManagerTest : public LockManagerTest {
 protected:
  void SetUp() override {
    saved_enabled_ = config::enable_key_range_locking();
    config::set_enable_key_range_locking(true);
  }
  void TearDown() override {
    config::set_enable_key_range_locking(saved_enabled_);
  }

 private:
  bool saved_enabled_ = false;
};

TEST_F(LockManagerTest, SingleTransactionAcquiresLock) {
  std::unique_ptr<LockHandle> lh =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh->EnqueueLock(request());
  EXPECT_FALSE(lh->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh->Wait());
}

TEST_F(LockManagerTest, ConcurrentTransactionIsAborted) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(2),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));

  // First transaction gets the lock.
  lh1->EnqueueLock(request());
  EXPECT_FALSE(lh1->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  EXPECT_FALSE(lh1->IsBlocked());
  EXPECT_FALSE(lh1->IsAborted());

  // Second transaction does not get the lock.
  lh2->EnqueueLock(request());
  EXPECT_FALSE(lh2->IsBlocked());
  EXPECT_TRUE(lh2->IsAborted());
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
  EXPECT_TRUE(lh2->IsAborted());
}

TEST_F(LockManagerTest, SequentialTransactionAcquiresLock) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(2),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  std::unique_ptr<LockHandle> lh3 =
      manager()->CreateHandle(TransactionID(3),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));

  // First transaction gets the lock.
  lh1->EnqueueLock(request());
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // Second transaction does not get the lock yet.
  lh2->EnqueueLock(request());
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  // First transaction unlocks.
  lh1->UnlockAll();

  // Second transaction is still in a final aborted state.
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  // Now another transaction can get the lock.
  lh3->EnqueueLock(request());
  GOOGLESQL_EXPECT_OK(lh3->Wait());
}

TEST_F(LockManagerTest, TransactionsThatDidNotAcquireLockCanReleaseIt) {
  std::unique_ptr<LockHandle> lh1 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh1->UnlockAll();

  std::unique_ptr<LockHandle> lh2 =
      manager()->CreateHandle(TransactionID(1),
                              /*try_abort_fn=*/nullptr, TransactionPriority(1));
  lh2->EnqueueLock(request());
  EXPECT_FALSE(lh2->IsBlocked());
  GOOGLESQL_EXPECT_OK(lh2->Wait());
}

// With the flag at its default, the whole-database lock is kept: a second
// read-write transaction is aborted even when its keys are disjoint.
TEST_F(LockManagerTest, DefaultKeepsSingleTransactionLock) {
  ASSERT_FALSE(config::enable_key_range_locking());
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(Write("table", 20, 30));
  EXPECT_FALSE(lh1->IsAborted());
  EXPECT_THAT(lh2->Wait(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kAborted,
                  testing::HasSubstr("one transaction at a time")));
}

// With the flag at its default, a second committer is aborted at once rather
// than waiting for the commit-timestamp reservation.
TEST_F(LockManagerTest, DefaultAbortsSecondCommitterImmediately) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());
  EXPECT_THAT(lh2->ReserveCommitTimestamp().status(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));
}

TEST_F(KeyRangeLockManagerTest, DisjointRangesOnSameTableDoNotConflict) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // [20,30) shares no key with [0,10): this is the case the whole-database
  // lock aborted and production Spanner runs concurrently.
  lh2->EnqueueLock(Write("table", 20, 30));
  const std::string diag = Scenario(
      "disjoint ranges on one table must both be granted",
      "exclusive table[0,10)", lh1.get(), "exclusive table[20,30)", lh2.get());
  GOOGLESQL_EXPECT_OK(lh2->Wait()) << diag;
  EXPECT_FALSE(lh1->IsAborted()) << diag;
  EXPECT_FALSE(lh2->IsAborted()) << diag;
}

TEST_F(KeyRangeLockManagerTest, DifferentTablesDoNotConflict) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("orders", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // Identical range, different table.
  lh2->EnqueueLock(Write("cards", 0, 10));
  const std::string diag = Scenario(
      "same range on different tables must both be granted",
      "exclusive orders[0,10)", lh1.get(), "exclusive cards[0,10)", lh2.get());
  GOOGLESQL_EXPECT_OK(lh2->Wait()) << diag;
  EXPECT_FALSE(lh1->IsAborted()) << diag;
}

TEST_F(KeyRangeLockManagerTest, SharedLocksOnSameRangeDoNotConflict) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Read("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // Two readers of the same rows: never a conflict in real Spanner.
  lh2->EnqueueLock(Read("table", 0, 10));
  const std::string diag = Scenario(
      "two shared locks on one range must both be granted",
      "shared table[0,10)", lh1.get(), "shared table[0,10)", lh2.get());
  GOOGLESQL_EXPECT_OK(lh2->Wait()) << diag;
  EXPECT_FALSE(lh1->IsAborted()) << diag;
  EXPECT_FALSE(lh2->IsAborted()) << diag;
}

TEST_F(KeyRangeLockManagerTest, OverlappingRangesConflictWhenOneIsExclusive) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // [5,15) overlaps [0,10) on keys 5..9. Exactly one side must survive.
  lh2->EnqueueLock(Write("table", 5, 15));
  const std::string diag = Scenario(
      "overlapping exclusive ranges: exactly one side must lose",
      "exclusive table[0,10)", lh1.get(), "exclusive table[5,15)", lh2.get());
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted())
      << "neither was aborted, so both hold overlapping exclusive ranges" << diag;
  EXPECT_FALSE(lh1->IsAborted() && lh2->IsAborted())
      << "both were aborted; the conflict should cost only one side" << diag;
}

TEST_F(KeyRangeLockManagerTest, SharedAndExclusiveOnSameRangeConflict) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Read("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  lh2->EnqueueLock(Write("table", 0, 10));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, AdjacentRangesDoNotConflict) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  // [0,10) and [10,20) touch but share no key -- the off-by-one that a
  // half-open interval comparison gets wrong if written with <=.
  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(Write("table", 10, 20));
  const std::string diag = Scenario(
      "adjacent half-open ranges share no key and must not conflict",
      "exclusive table[0,10)", lh1.get(), "exclusive table[10,20)", lh2.get());
  GOOGLESQL_EXPECT_OK(lh2->Wait()) << diag;
  EXPECT_FALSE(lh1->IsAborted())
      << "a '<=' in the overlap test would produce exactly this" << diag;
}

// Lock shapes a keyed read takes: a point per full key, a prefix range per
// leading-key match.
TEST_F(KeyRangeLockManagerTest, SharedPointReadDoesNotConflictWithWriteToOtherPoint) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(
      LockRequest(LockMode::kShared, "table", KeyRange::Point(K(1)), {}));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(
      LockRequest(LockMode::kExclusive, "table", KeyRange::Point(K(2)), {}));
  GOOGLESQL_EXPECT_OK(lh2->Wait());
  EXPECT_FALSE(lh1->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, SharedPointReadConflictsWithWriteToSamePoint) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(
      LockRequest(LockMode::kShared, "table", KeyRange::Point(K(1)), {}));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(
      LockRequest(LockMode::kExclusive, "table", KeyRange::Point(K(1)), {}));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, PrefixReadConflictsWithWriteInsideThePrefix) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  // Covers a row that does not exist yet: the phantom an insert would create.
  lh1->EnqueueLock(
      LockRequest(LockMode::kShared, "table", KeyRange::Prefix(K(5)), {}));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(LockRequest(LockMode::kExclusive, "table",
                               KeyRange::Point(Key({Int64(5), Int64(7)})),
                               {}));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, PrefixReadDoesNotConflictWithWriteOutsideThePrefix) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(
      LockRequest(LockMode::kShared, "table", KeyRange::Prefix(K(5)), {}));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh2->EnqueueLock(LockRequest(LockMode::kExclusive, "table",
                               KeyRange::Point(Key({Int64(6), Int64(0)})),
                               {}));
  GOOGLESQL_EXPECT_OK(lh2->Wait());
  EXPECT_FALSE(lh1->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, UnboundedRangeConflictsWithEverything) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  // A full-table scan must be treated as covering every key: reporting it as
  // disjoint would let a genuine race pass locally.
  lh1->EnqueueLock(
      LockRequest(LockMode::kExclusive, "table", KeyRange::All(), {}));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  lh2->EnqueueLock(Write("table", 1000, 2000));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, SameTransactionNeverConflictsWithItself) {
  std::unique_ptr<LockHandle> lh = Handle(1);

  lh->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh->Wait());
  // Re-locking an overlapping range it already holds must always be granted.
  lh->EnqueueLock(Write("table", 5, 15));
  GOOGLESQL_EXPECT_OK(lh->Wait());
  EXPECT_FALSE(lh->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, UnlockAllReleasesRangesForReuse) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh1->UnlockAll();

  // If UnlockAll leaked the range, this same range would stay blocked for the
  // life of the process.
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh2->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh2->Wait());
  EXPECT_FALSE(lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, CommitReservationIsReleasedByMarkCommitted) {
  // If only UnlockAll cleared the reservation, a transaction would stay
  // "committing" across its caller's flush to storage and abort any peer
  // committing in that window.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());
  GOOGLESQL_EXPECT_OK(lh1->MarkCommitted());

  // lh1 has committed but has NOT yet called UnlockAll -- exactly the window
  // the caller spends flushing. A peer on disjoint keys must still commit.
  lh2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(lh2->Wait());
  GOOGLESQL_EXPECT_OK(lh2->ReserveCommitTimestamp().status());
  GOOGLESQL_EXPECT_OK(lh2->MarkCommitted());
  EXPECT_FALSE(lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, CommitTimestampsAreMonotonicUnderConcurrency) {
  // Commit ordering must stay serialised even though acquisition no longer is:
  // a read at a timestamp is only meaningful if commits are ordered.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  lh2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh2->Wait());

  auto ts1 = lh1->ReserveCommitTimestamp();
  GOOGLESQL_EXPECT_OK(ts1.status());
  GOOGLESQL_EXPECT_OK(lh1->MarkCommitted());

  auto ts2 = lh2->ReserveCommitTimestamp();
  GOOGLESQL_EXPECT_OK(ts2.status());
  GOOGLESQL_EXPECT_OK(lh2->MarkCommitted());

  EXPECT_LT(*ts1, *ts2);
}

TEST_F(KeyRangeLockManagerTest, ConcurrentCommitReservationWaitsForTheHolder) {
  // A committer arriving while a peer holds the reservation must WAIT for it,
  // not abort. The peer's window is only its flush, so waiting is far cheaper
  // than the retry an abort forces -- and the Spanner client does not retry
  // ABORTED at commit at all.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh1->EnqueueLock(Write("table", 0, 10));
  lh2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh2->Wait());

  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());

  // lh2 blocks here until lh1 marks committed on another thread.
  absl::StatusOr<absl::Time> second;
  std::thread committer([&] { second = lh2->ReserveCommitTimestamp(); });
  absl::SleepFor(absl::Milliseconds(50));  // let lh2 reach the wait
  GOOGLESQL_EXPECT_OK(lh1->MarkCommitted());
  committer.join();

  GOOGLESQL_EXPECT_OK(second.status())
      << "second committer should have been granted once the holder released";
  GOOGLESQL_EXPECT_OK(lh2->MarkCommitted());
}

TEST_F(KeyRangeLockManagerTest, CommitReservationWaitIsBoundedAndThenAborts) {
  // The wait must never hang. With the budget at zero the old abort-at-once
  // behaviour is restored, which is also the fallback when the budget expires.
  const int saved = config::commit_reservation_wait_ms();
  config::set_commit_reservation_wait_ms(0);

  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh1->EnqueueLock(Write("table", 0, 10));
  lh2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh2->Wait());

  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());
  EXPECT_THAT(lh2->ReserveCommitTimestamp().status(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAborted));

  config::set_commit_reservation_wait_ms(saved);
}

TEST_F(KeyRangeLockManagerTest, UnlockAllWakesAWaitingCommitter) {
  // A holder that abandons its commit (rollback / destructor) releases the
  // reservation through UnlockAll, which must wake a waiter just as
  // MarkCommitted does.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh1->EnqueueLock(Write("table", 0, 10));
  lh2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh2->Wait());

  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());
  absl::StatusOr<absl::Time> second;
  std::thread committer([&] { second = lh2->ReserveCommitTimestamp(); });
  absl::SleepFor(absl::Milliseconds(50));
  lh1->UnlockAll();  // abandon, never MarkCommitted
  committer.join();

  GOOGLESQL_EXPECT_OK(second.status());
}

TEST_F(KeyRangeLockManagerTest, RangesSurviveMarkCommittedUntilUnlockAll) {
  // MarkCommitted releases only the commit-timestamp reservation. The ranges
  // must stay held until UnlockAll, or a peer could write keys this
  // transaction still owns while its caller is mid-flush.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);

  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  GOOGLESQL_EXPECT_OK(lh1->ReserveCommitTimestamp().status());
  GOOGLESQL_EXPECT_OK(lh1->MarkCommitted());

  // Overlapping range, before lh1 unlocks: one side must still lose.
  lh2->EnqueueLock(Write("table", 5, 15));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, RepeatedIdenticalLockDoesNotAccumulate) {
  // Every row op takes a Point lock, so the same range is requested repeatedly.
  // Storing a duplicate each time would grow the table without bound.
  std::unique_ptr<LockHandle> lh = Handle(1);
  for (int i = 0; i < 50; ++i) {
    lh->EnqueueLock(Write("table", 0, 10));
    GOOGLESQL_EXPECT_OK(lh->Wait());
  }
  // Observable proof the entries were collapsed: releasing once frees the range
  // for a peer. If 50 entries had accumulated, ReleaseLocksFor still drops them
  // all by tid, so this also guards against a partial release.
  lh->UnlockAll();
  std::unique_ptr<LockHandle> peer = Handle(2);
  peer->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(peer->Wait());
  EXPECT_FALSE(peer->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, SharedLockUpgradesToExclusiveOnSameRange) {
  // A transaction that reads a row and then writes it must end up holding the
  // range exclusively -- collapsing the two must not leave it shared, or a
  // concurrent reader would wrongly be allowed in.
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  lh1->EnqueueLock(Read("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());
  lh1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(lh1->Wait());

  // The range is now exclusive, so another reader must conflict.
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh2->EnqueueLock(Read("table", 0, 10));
  EXPECT_TRUE(lh1->IsAborted() || lh2->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, DatabaseWideLockConflictsWithEveryTable) {
  // A schema change takes a "database-wide lock" as table_id="" with
  // KeyRange::All() (scoped_schema_change_lock.h). Matching on table id alone
  // would let DDL run concurrently with DML on a real table -- the schema
  // change would stop excluding the writes it is altering.
  std::unique_ptr<LockHandle> ddl = Handle(1);
  ddl->EnqueueLock(
      LockRequest(LockMode::kExclusive, /*table_id=*/"", KeyRange::All(), {}));
  GOOGLESQL_EXPECT_OK(ddl->Wait());

  std::unique_ptr<LockHandle> dml = Handle(2);
  dml->EnqueueLock(Write("orders", 0, 10));
  EXPECT_TRUE(ddl->IsAborted() || dml->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, DatabaseWideLockConflictsWhenRequestedSecond) {
  // The same in the other direction: a data transaction already holding a range
  // must block a schema change.
  std::unique_ptr<LockHandle> dml = Handle(1);
  dml->EnqueueLock(Write("orders", 0, 10));
  GOOGLESQL_EXPECT_OK(dml->Wait());

  std::unique_ptr<LockHandle> ddl = Handle(2);
  ddl->EnqueueLock(
      LockRequest(LockMode::kExclusive, /*table_id=*/"", KeyRange::All(), {}));
  EXPECT_TRUE(dml->IsAborted() || ddl->IsAborted());
}

TEST_F(KeyRangeLockManagerTest, DestroyedHandleDoesNotLeaveADanglingEntry) {
  // The manager stores a raw LockHandle* per range. A handle destroyed without
  // UnlockAll would leave a pointer that FindConflict dereferences, so the
  // destructor releases the ranges itself.
  {
    std::unique_ptr<LockHandle> doomed = Handle(1);
    doomed->EnqueueLock(Write("table", 0, 10));
    GOOGLESQL_EXPECT_OK(doomed->Wait());
    // No UnlockAll: the handle goes out of scope holding the range.
  }

  // Same range: reaches the freed entry if it was left behind.
  std::unique_ptr<LockHandle> next = Handle(2);
  next->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(next->Wait());
  EXPECT_FALSE(next->IsAborted());
}

// ---------------------------------------------------------------------------
// Multi-blocker preemption. Pin the coin flip to 100 so the
// preempt path is taken deterministically, and restore it afterwards.
// ---------------------------------------------------------------------------

class PreemptingLockManagerTest : public KeyRangeLockManagerTest {
 protected:
  void SetUp() override {
    KeyRangeLockManagerTest::SetUp();
    saved_probability_ = config::abort_current_transaction_probability();
    config::set_abort_current_transaction_probability(100);
  }
  void TearDown() override {
    config::set_abort_current_transaction_probability(saved_probability_);
    KeyRangeLockManagerTest::TearDown();
  }

 private:
  int saved_probability_ = 0;
};

TEST_F(PreemptingLockManagerTest, ExclusiveOverTwoSharedHoldersWoundsBoth) {
  // T1 and T2 read [0,10). T3 wants to write it. Granting T3 after wounding
  // only T1 would leave T2's shared lock co-resident with T3's exclusive one:
  // T2 has read rows T3 overwrites, both commit, and the lost update that real
  // Spanner aborts slips through.
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = AbortableHandle(2);
  std::unique_ptr<LockHandle> t3 = AbortableHandle(3);

  t1->EnqueueLock(Read("table", 0, 10));
  t2->EnqueueLock(Read("table", 0, 10));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  GOOGLESQL_EXPECT_OK(t2->Wait());

  t3->EnqueueLock(Write("table", 0, 10));
  const std::string diag = Scenario(
      "exclusive over two shared holders: every holder must be wounded",
      "shared table[0,10)", t1.get(), "shared table[0,10)", t2.get());

  GOOGLESQL_EXPECT_OK(t3->Wait()) << "requester should be granted" << diag;
  EXPECT_TRUE(t1->IsAborted()) << diag;
  EXPECT_TRUE(t2->IsAborted())
      << "second shared holder survived alongside an exclusive lock" << diag;
}

TEST_F(PreemptingLockManagerTest, ExclusiveOverTwoRangesWoundsBothHolders) {
  // The same shape across two disjoint holders covered by one wide request:
  // T1 holds [0,10), T2 holds [10,20), T3 wants [0,20).
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = AbortableHandle(2);
  std::unique_ptr<LockHandle> t3 = AbortableHandle(3);

  t1->EnqueueLock(Write("table", 0, 10));
  t2->EnqueueLock(Write("table", 10, 20));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  GOOGLESQL_EXPECT_OK(t2->Wait());

  t3->EnqueueLock(Write("table", 0, 20));
  GOOGLESQL_EXPECT_OK(t3->Wait());
  EXPECT_TRUE(t1->IsAborted());
  EXPECT_TRUE(t2->IsAborted());
}

TEST_F(PreemptingLockManagerTest, RequesterLosesIfAnyBlockerCannotBeWounded) {
  // T1 is abortable, T2 is not (null callback). T3 must NOT be granted: with
  // T2 unwoundable the request cannot be made safe, so the requester is the one
  // aborted. T1 having been wounded first is conservative and acceptable.
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = Handle(2);
  std::unique_ptr<LockHandle> t3 = AbortableHandle(3);

  t1->EnqueueLock(Read("table", 0, 10));
  t2->EnqueueLock(Read("table", 0, 10));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  GOOGLESQL_EXPECT_OK(t2->Wait());

  t3->EnqueueLock(Write("table", 0, 10));
  EXPECT_TRUE(t3->IsAborted())
      << "requester was granted while an unwoundable shared holder remains";
  EXPECT_FALSE(t2->IsAborted()) << "an unwoundable holder must survive";
}

TEST_F(PreemptingLockManagerTest, ConflictErrorNamesTheRangeNotTheOldLimit) {
  // A genuine range conflict must not read like the old whole-database limit,
  // or a failure cannot tell "real overlap" from "old image".
  std::unique_ptr<LockHandle> t1 = Handle(1);
  std::unique_ptr<LockHandle> t2 = Handle(2);
  t1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  t2->EnqueueLock(Write("table", 0, 10));
  absl::Status status = t2->Wait();
  EXPECT_EQ(status.code(), absl::StatusCode::kAborted);
  EXPECT_THAT(std::string(status.message()),
              testing::HasSubstr("conflicting lock on table 'table'"));
  EXPECT_THAT(std::string(status.message()),
              testing::Not(testing::HasSubstr("one transaction at a time")));
}

TEST_F(PreemptingLockManagerTest, WoundedHolderErrorNamesTheRule) {
  // Under key-range locking a preempted holder lost an overlapping range, not
  // the database-wide single-transaction slot.
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = AbortableHandle(2);
  t1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  t2->EnqueueLock(Write("table", 5, 15));
  GOOGLESQL_EXPECT_OK(t2->Wait());
  absl::Status status = t1->Wait();
  EXPECT_EQ(status.code(), absl::StatusCode::kAborted);
  EXPECT_THAT(std::string(status.message()),
              testing::HasSubstr("overlaps one this transaction holds"));
  EXPECT_THAT(std::string(status.message()),
              testing::HasSubstr("Rule: random preemption."));
  EXPECT_THAT(std::string(status.message()),
              testing::Not(testing::HasSubstr("one transaction at a time")));
}

TEST_F(LockManagerTest, DefaultWoundedHolderKeepsUpstreamMessage) {
  ASSERT_FALSE(config::enable_key_range_locking());
  int saved_probability = config::abort_current_transaction_probability();
  config::set_abort_current_transaction_probability(100);
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = AbortableHandle(2);
  t1->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(t1->Wait());
  t2->EnqueueLock(Write("table", 20, 30));
  GOOGLESQL_EXPECT_OK(t2->Wait());
  EXPECT_THAT(t1->Wait(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kAborted,
                  testing::HasSubstr(
                      "getting priority. The emulator only supports one "
                      "transaction at a time.")));
  config::set_abort_current_transaction_probability(saved_probability);
}

// --lock_conflict_policy=wound_wait, which applies only with key-range locking.
class WoundWaitLockManagerTest : public KeyRangeLockManagerTest {
 protected:
  void SetUp() override {
    KeyRangeLockManagerTest::SetUp();
    saved_policy_ = config::lock_conflict_policy();
    config::set_lock_conflict_policy(config::LockConflictPolicy::kWoundWait);
  }
  void TearDown() override {
    config::set_lock_conflict_policy(saved_policy_);
    KeyRangeLockManagerTest::TearDown();
  }

 private:
  config::LockConflictPolicy saved_policy_ = config::LockConflictPolicy::kRandom;
};

TEST_F(WoundWaitLockManagerTest, OlderRequesterWoundsYoungerHolder) {
  std::unique_ptr<LockHandle> older = AbortableHandle(1);
  std::unique_ptr<LockHandle> younger = AbortableHandle(2);
  younger->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(younger->Wait());

  older->EnqueueLock(Write("table", 0, 10));
  const std::string diag =
      Scenario("older requester over younger holder", "write table[0,10)",
               older.get(), "write table[0,10)", younger.get());
  GOOGLESQL_EXPECT_OK(older->Wait()) << diag;
  ASSERT_TRUE(younger->IsAborted()) << diag;
  EXPECT_THAT(std::string(younger->Wait().message()),
              testing::HasSubstr("Rule: wound-wait"));
}

TEST_F(WoundWaitLockManagerTest, YoungerRequesterIsAbortedByOlderHolder) {
  std::unique_ptr<LockHandle> older = AbortableHandle(1);
  std::unique_ptr<LockHandle> younger = AbortableHandle(2);
  older->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(older->Wait());

  younger->EnqueueLock(Read("table", 5, 6));
  ASSERT_TRUE(younger->IsAborted());
  EXPECT_FALSE(older->IsAborted()) << "an older holder must never be wounded";
  const std::string message(younger->Wait().message());
  EXPECT_THAT(message, testing::HasSubstr("conflicting lock on table 'table'"));
  EXPECT_THAT(message, testing::HasSubstr("younger requester"));
}

TEST_F(WoundWaitLockManagerTest, PriorityDecidesAgeBeforeTransactionId) {
  // Priority is the begin time a retry keeps, so a retried transaction with a
  // newer id is still the older one.
  std::unique_ptr<LockHandle> holder = AbortableHandle(5, /*priority=*/200);
  std::unique_ptr<LockHandle> retried = AbortableHandle(9, /*priority=*/100);
  holder->EnqueueLock(Write("table", 0, 10));
  retried->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(retried->Wait());
  EXPECT_TRUE(holder->IsAborted());
}

TEST_F(WoundWaitLockManagerTest, RequesterYoungerThanAnyHolderIsAborted) {
  // T1 (oldest) and T3 (youngest) share [0,10); T2 is older than T3 only.
  std::unique_ptr<LockHandle> t1 = AbortableHandle(1);
  std::unique_ptr<LockHandle> t2 = AbortableHandle(2);
  std::unique_ptr<LockHandle> t3 = AbortableHandle(3);
  t1->EnqueueLock(Read("table", 0, 10));
  t3->EnqueueLock(Read("table", 0, 10));

  t2->EnqueueLock(Write("table", 0, 10));
  EXPECT_TRUE(t2->IsAborted());
  EXPECT_FALSE(t1->IsAborted());
  EXPECT_FALSE(t3->IsAborted()) << "no holder is wounded unless all can be";
}

TEST_F(WoundWaitLockManagerTest, OlderRequesterLosesIfHolderCannotBeWounded) {
  std::unique_ptr<LockHandle> older = AbortableHandle(1);
  std::unique_ptr<LockHandle> younger = Handle(2);  // e.g. mid-commit
  younger->EnqueueLock(Write("table", 0, 10));
  older->EnqueueLock(Write("table", 0, 10));
  ASSERT_TRUE(older->IsAborted());
  EXPECT_FALSE(younger->IsAborted());
  EXPECT_THAT(std::string(older->Wait().message()),
              testing::HasSubstr("could not be wounded"));
}

TEST_F(WoundWaitLockManagerTest, SchemaChangeRequestNeverWounds) {
  std::unique_ptr<LockHandle> reader = AbortableHandle(2);
  std::unique_ptr<LockHandle> ddl = AbortableHandle(1, /*priority=*/0);
  reader->EnqueueLock(Read("table", 0, 10));
  ddl->EnqueueLock(LockRequest(LockMode::kExclusive, "", KeyRange::All(), {}));
  EXPECT_TRUE(ddl->IsAborted());
  EXPECT_FALSE(reader->IsAborted());
}

TEST_F(WoundWaitLockManagerTest, OutcomeIsDeterministicAcrossRuns) {
  for (int run = 0; run < 200; ++run) {
    LockManager fresh_manager(clock());
    std::unique_ptr<LockHandle> older = fresh_manager.CreateHandle(
        TransactionID(1), []() { return absl::OkStatus(); },
        TransactionPriority(1));
    std::unique_ptr<LockHandle> younger = fresh_manager.CreateHandle(
        TransactionID(2), []() { return absl::OkStatus(); },
        TransactionPriority(1));
    older->EnqueueLock(Write("table", 0, 10));
    younger->EnqueueLock(Write("table", 0, 10));
    ASSERT_FALSE(older->IsAborted()) << "run " << run;
    ASSERT_TRUE(younger->IsAborted()) << "run " << run;
  }
}

class RandomPolicyLockManagerTest : public KeyRangeLockManagerTest {
 protected:
  void SetUp() override {
    KeyRangeLockManagerTest::SetUp();
    saved_policy_ = config::lock_conflict_policy();
    saved_probability_ = config::abort_current_transaction_probability();
    config::set_lock_conflict_policy(config::LockConflictPolicy::kRandom);
  }
  void TearDown() override {
    config::set_lock_conflict_policy(saved_policy_);
    config::set_abort_current_transaction_probability(saved_probability_);
    KeyRangeLockManagerTest::TearDown();
  }

 private:
  config::LockConflictPolicy saved_policy_ = config::LockConflictPolicy::kRandom;
  int saved_probability_ = 0;
};

TEST_F(RandomPolicyLockManagerTest, ZeroProbabilityAbortsEvenAnOlderRequester) {
  config::set_abort_current_transaction_probability(0);
  std::unique_ptr<LockHandle> older = AbortableHandle(1);
  std::unique_ptr<LockHandle> younger = AbortableHandle(2);
  younger->EnqueueLock(Write("table", 0, 10));
  older->EnqueueLock(Write("table", 0, 10));
  ASSERT_TRUE(older->IsAborted()) << "age is ignored under the random policy";
  EXPECT_FALSE(younger->IsAborted());
  EXPECT_THAT(std::string(older->Wait().message()),
              testing::HasSubstr("Rule: random preemption"));
}

TEST_F(RandomPolicyLockManagerTest, FullProbabilityLetsAYoungerRequesterWound) {
  config::set_abort_current_transaction_probability(100);
  std::unique_ptr<LockHandle> older = AbortableHandle(1);
  std::unique_ptr<LockHandle> younger = AbortableHandle(2);
  older->EnqueueLock(Write("table", 0, 10));
  younger->EnqueueLock(Write("table", 0, 10));
  GOOGLESQL_EXPECT_OK(younger->Wait());
  ASSERT_TRUE(older->IsAborted());
  EXPECT_THAT(std::string(older->Wait().message()),
              testing::HasSubstr("Rule: random preemption"));
}

TEST_F(LockManagerTest, DefaultPolicyIsRandom) {
  EXPECT_EQ(config::lock_conflict_policy(), config::LockConflictPolicy::kRandom);
}

TEST_F(LockManagerTest, WoundWaitIsIgnoredWithoutKeyRangeLocking) {
  const config::LockConflictPolicy saved_policy = config::lock_conflict_policy();
  config::set_lock_conflict_policy(config::LockConflictPolicy::kWoundWait);
  std::unique_ptr<LockHandle> older = Handle(1);
  std::unique_ptr<LockHandle> younger = Handle(2);
  younger->EnqueueLock(Write("table", 0, 10));
  older->EnqueueLock(Write("table", 90, 100));
  // The database-wide lock still aborts the requester, whatever its age.
  EXPECT_TRUE(older->IsAborted());
  EXPECT_FALSE(younger->IsAborted());
  config::set_lock_conflict_policy(saved_policy);
}

TEST_F(LockManagerTest, EnsuresSerializationWithParallelTransactions) {
  // Simulate a thread-safe mvcc store with a single key. Even though multiple
  // threads access this store, they are synchronized by the lock manager.
  // Concurrent access issues are expected to be caught by tsan.
  std::map<absl::Time, int> value{{absl::InfinitePast(), 0}};
  auto SetValue = [&value](absl::Time t, int i) { value[t] = i; };
  auto GetValue = [&value](absl::Time t) {
    auto itr = value.upper_bound(t);
    --itr;
    return itr->second;
  };

  // Start n threads each doing a transactional increment k times.
  int n = 20;
  int k = 10;
  std::vector<std::thread> threads;
  std::atomic<int> id_counter(0);
  for (int i = 0; i < n; ++i) {
    threads.emplace_back(
        [&](int i) {
          for (int j = 0; j < k; ++j) {
            while (true) {
              // Get a lock.
              std::unique_ptr<LockHandle> lh = manager()->CreateHandle(
                  TransactionID(++id_counter),
                  /*try_abort_fn=*/nullptr, TransactionPriority(1));
              lh->EnqueueLock(request());
              absl::Status status = lh->Wait();

              // Retry on aborts.
              if (status.code() == absl::StatusCode::kAborted) {
                continue;
              } else {
                GOOGLESQL_ASSERT_OK(status);
              }

              // We got the lock, increment the counter.
              int cur_value = GetValue(absl::InfiniteFuture());
              int new_value = cur_value + 1;
              SetValue(clock()->Now(), new_value);

              // Unlock the lock.
              lh->UnlockAll();
              break;
            }
          }
        },
        i);
  }

  // Wait for all threads to complete.
  for (std::thread& thread : threads) {
    thread.join();
  }

  // Expect that the counter was incremented n*k times.
  EXPECT_EQ(n * k, GetValue(absl::InfiniteFuture()));
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
