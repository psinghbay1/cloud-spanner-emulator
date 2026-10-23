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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/random/uniform_int_distribution.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/request.h"
#include "common/config.h"
#include "common/errors.h"
#include "googlesql/base/ret_check.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Names the conflict rule in abort messages under key-range locking.
constexpr absl::string_view kRandomPreemptionRule = "random preemption";

// True when two key ranges share at least one key.
//
// Normalising to closed-open makes this a plain interval comparison:
// [a,b) and [c,d) overlap iff a < d and c < b. Key::Empty() sorts below every
// key and Key::Infinity() above every key, so KeyRange::All() -- which is
// [Empty, Infinity) -- overlaps everything without needing a special case.
bool RangesOverlap(const KeyRange& lhs, const KeyRange& rhs) {
  const KeyRange l = lhs.ToClosedOpen();
  const KeyRange r = rhs.ToClosedOpen();
  return l.start_key() < r.limit_key() && r.start_key() < l.limit_key();
}

// Two requests conflict when they touch the same table, their ranges overlap,
// and at least one is exclusive. Concurrent readers of the same rows do not
// conflict, which is what real Spanner does and what the whole-database lock
// could not express.
//
// Conservative by construction: anything this reports as non-conflicting really
// is disjoint. A false conflict costs a retry; a false non-conflict would let a
// genuine data race pass locally, which defeats the emulator's purpose.
bool Conflicts(LockMode held_mode, const TableID& held_table,
               const KeyRange& held_range, const LockRequest& request) {
  // An empty table id is the database-wide lock a schema change takes
  // (scoped_schema_change_lock.h uses table_id="" with KeyRange::All() and
  // calls it "dummy arguments to represent a database-wide lock"). It must
  // conflict with every table, or DDL would stop excluding DML and a schema
  // change could run concurrently with writes to the tables it is altering.
  const bool database_wide =
      held_table.empty() || request.table_id().empty();
  if (!database_wide && held_table != request.table_id()) return false;
  if (held_mode == LockMode::kShared && request.mode() == LockMode::kShared) {
    return false;
  }
  return RangesOverlap(held_range, request.key_range());
}

// True when the range covers every key of the table.
bool IsWholeTable(const KeyRange& range) {
  const KeyRange closed_open = range.ToClosedOpen();
  return closed_open.start_key() == Key::Empty() &&
         closed_open.limit_key() == Key::Infinity();
}

}  // namespace

std::unique_ptr<LockHandle> LockManager::CreateHandle(
    TransactionID tid, const std::function<absl::Status()>& abort_fn,
    TransactionPriority priority) {
  return absl::WrapUnique(new LockHandle(this, tid, abort_fn, priority));
}

// Records a granted lock, collapsing an exact repeat.
//
// Every row-level read and write takes a KeyRange::Point lock
// (transaction_store.cc), so a transaction re-touching the same row would
// otherwise add an entry each time. The scan is linear per lock request, so
// entries cost time under mu_ -- measured at ~136us for 20k distinct ranges in
// one transaction, which is acceptable, but there is no reason to store the
// same range twice.
//
// Only exact repeats are collapsed. Merging adjacent or contained ranges would
// need care to stay conservative, and the win here comes from the repeated
// identical Point locks a single row generates.
void LockManager::RecordLock(LockHandle* handle, const LockRequest& request) {
  for (HeldLock& held : held_locks_) {
    if (held.tid != handle->tid()) continue;
    if (held.table_id != request.table_id()) continue;
    if (!(held.key_range == request.key_range())) continue;
    // Upgrade shared to exclusive; never downgrade.
    if (held.mode == LockMode::kShared &&
        request.mode() == LockMode::kExclusive) {
      held.mode = LockMode::kExclusive;
    }
    return;
  }
  held_locks_.push_back({handle->tid(), handle, request.table_id(),
                         request.key_range(), request.mode()});
}

std::vector<LockHandle*> LockManager::FindConflicts(
    LockHandle* handle, const LockRequest& request) {
  std::vector<LockHandle*> blockers;
  for (const HeldLock& held : held_locks_) {
    // A transaction never conflicts with itself.
    if (held.tid == handle->tid()) continue;
    if (!Conflicts(held.mode, held.table_id, held.key_range, request)) continue;
    // One entry per transaction, however many of its ranges overlap.
    bool seen = false;
    for (LockHandle* b : blockers) {
      if (b->tid() == held.tid) { seen = true; break; }
    }
    if (!seen) blockers.push_back(held.handle);
  }
  return blockers;
}

void LockManager::RecordConflict(LockHandle* handle,
                                 const LockRequest& request) {
  bool whole_table = IsWholeTable(request.key_range());
  for (const HeldLock& held : held_locks_) {
    if (held.tid == handle->tid()) continue;
    if (!Conflicts(held.mode, held.table_id, held.key_range, request)) continue;
    if (IsWholeTable(held.key_range)) whole_table = true;
  }
  LockConflictStats& stats = conflict_stats_[request.table_id()];
  if (whole_table) {
    ++stats.whole_table_conflicts;
  } else {
    ++stats.bounded_conflicts;
  }
  if (request.mode() == LockMode::kExclusive) {
    ++stats.exclusive_conflicts;
  } else {
    ++stats.shared_conflicts;
  }
  stats.last_conflict_time = absl::Now();
}

std::map<TableID, LockConflictStats> LockManager::ConflictStats() {
  absl::MutexLock lock(mu_);
  return conflict_stats_;
}

void LockManager::ReleaseLocksFor(TransactionID tid) {
  std::erase_if(held_locks_,
                [tid](const HeldLock& held) { return held.tid == tid; });
}

void LockManager::EnqueueLock(LockHandle* handle, const LockRequest& request) {
  absl::MutexLock lock(mu_);

  // Don't hand out locks to aborted handles.
  if (handle->IsAborted()) {
    return;
  }

  // Off by default: the whole-database lock, one read-write transaction at a
  // time.
  if (!config::enable_key_range_locking()) {
    EnqueueDatabaseLock(handle);
    return;
  }

  std::vector<LockHandle*> blockers = FindConflicts(handle, request);
  if (blockers.empty()) {
    RecordLock(handle, request);
    return;
  }
  RecordConflict(handle, request);
  ResolveConflict(handle, request, blockers);
}

// Wounds every blocker and grants the request, or returns false if one cannot
// be wounded (e.g. mid-commit). All of them: wounding one of two shared holders
// would leave the other co-resident with the granted exclusive lock.
bool LockManager::WoundAll(LockHandle* handle, const LockRequest& request,
                           const std::vector<LockHandle*>& blockers) {
  for (LockHandle* blocker : blockers) {
    if (!blocker->TryAbortTransaction(
                     error::AbortCurrentTransaction(
                         blocker->tid(), handle->tid(), kRandomPreemptionRule))
             .ok()) {
      return false;
    }
    ReleaseLocksFor(blocker->tid());
  }
  RecordLock(handle, request);
  return true;
}

// The same rule as the whole-database lock: preempt the holders with
// --abort_current_transaction_probability, else abort the requester, so a
// long-running holder cannot starve newcomers.
void LockManager::ResolveConflict(
    LockHandle* handle, const LockRequest& request,
    const std::vector<LockHandle*>& blockers) {
  absl::BitGen gen;
  if (absl::uniform_int_distribution<int>(1, 100)(gen) <=
          config::abort_current_transaction_probability() &&
      WoundAll(handle, request, blockers)) {
    return;
  }
  handle->Abort(error::AbortConflictingKeyRange(
      handle->tid(), blockers.front()->tid(), request.table_id(),
      request.key_range().DebugString(), kRandomPreemptionRule));
}

// The pre-existing whole-database lock. Caller must hold mu_.
void LockManager::EnqueueDatabaseLock(LockHandle* handle) {
  if (active_handle_ == nullptr) {
    active_handle_ = handle;
    return;
  }
  if (active_handle_->tid() == handle->tid()) {
    return;
  }
  absl::BitGen gen;
  if (absl::uniform_int_distribution<int>(1, 100)(gen) <=
      config::abort_current_transaction_probability()) {
    auto could_be_aborted = active_handle_->TryAbortTransaction(
        error::AbortCurrentTransaction(active_handle_->tid(), handle->tid()));
    if (could_be_aborted.ok()) {
      active_handle_ = handle;
      return;
    }
  }
  handle->Abort(
      error::AbortConcurrentTransaction(handle->tid(), active_handle_->tid()));
}

void LockManager::UnlockAll(LockHandle* handle) {
  absl::MutexLock lock(mu_);

  // Release the ranges first, and unconditionally: a transaction can hold
  // ranges without being active_handle_, which is only set while a commit
  // timestamp is being reserved. Returning early here would leak every lock it
  // holds and block those keys for the life of the process.
  ReleaseLocksFor(handle->tid());

  if (active_handle_ != nullptr && active_handle_->tid() == handle->tid()) {
    active_handle_ = nullptr;
    // A committer may be waiting in ReserveCommitTimestamp for this.
    pending_commit_cvar_.SignalAll();
  }
  handle->Reset();
}

absl::StatusOr<absl::Time> LockManager::ReserveCommitTimestamp(
    LockHandle* handle) {
  absl::MutexLock lock(mu_);

  // Under key-range locking active_handle_ means "a commit timestamp is being
  // reserved", not "the one transaction allowed to run". Commit ordering stays
  // serialised -- it is what makes a read at a timestamp meaningful -- while
  // lock acquisition is concurrent.
  //
  // Transactions on unrelated keys then reach commit together, and many
  // clients do not retry ABORTED at commit. The reservation is held only
  // across the holder's flush and is always released (MarkCommitted, or
  // UnlockAll for an abandoned holder), so wait for it, bounded so a wedged
  // holder cannot hang a committer. With the flag off the wait is zero and a
  // second committer is aborted at once, as before.
  const absl::Duration budget =
      config::enable_key_range_locking()
          ? absl::Milliseconds(config::commit_reservation_wait_ms())
          : absl::ZeroDuration();
  const absl::Time deadline = absl::Now() + budget;
  while (active_handle_ != nullptr && active_handle_->tid() != handle->tid()) {
    if (budget <= absl::ZeroDuration() ||
        pending_commit_cvar_.WaitWithDeadline(&mu_, deadline)) {
      return error::AbortConcurrentTransaction(handle->tid(),
                                               active_handle_->tid());
    }
  }
  active_handle_ = handle;

  pending_commit_timestamp_ = clock_->Now();
  return pending_commit_timestamp_;
}

absl::Status LockManager::MarkCommitted(LockHandle* handle) {
  absl::MutexLock lock(mu_);

  // This transaction should hold the commit-timestamp reservation. Checked for
  // null first: MarkCommitted can clear active_handle_, so a second call would
  // otherwise dereference a null pointer.
  GOOGLESQL_RET_CHECK(active_handle_ != nullptr)
      << absl::Substitute("Transaction $0 is not active.", handle->tid());
  GOOGLESQL_RET_CHECK_EQ(active_handle_->tid(), handle->tid())
      << absl::Substitute("Transaction $0 is not active.", handle->tid());

  last_commit_timestamp_ = pending_commit_timestamp_;
  pending_commit_timestamp_ = absl::InfiniteFuture();

  // Under key-range locking the reservation ends here: leaving active_handle_
  // set until UnlockAll would abort any peer that commits during the caller's
  // flush to storage. The ranges stay held until UnlockAll. With the flag off,
  // active_handle_ is the database lock and is released by UnlockAll only.
  if (config::enable_key_range_locking()) {
    active_handle_ = nullptr;
  }

  pending_commit_cvar_.SignalAll();
  return absl::OkStatus();
}

void LockManager::WaitForSafeRead(absl::Time read_time) {
  absl::MutexLock lock(mu_);

  // Wait for read time to become current if passed a future timestamp  for the
  // case of exact timestamp bound for snapshot read.
  // https://cloud.google.com/spanner/docs/timestamp-bounds#introduction
  bool f = false;
  mu_.AwaitWithDeadline(absl::Condition(&f), read_time);

  while (pending_commit_timestamp_ < read_time) {
    pending_commit_cvar_.Wait(&mu_);
  }
}

absl::Time LockManager::LastCommitTimestamp() {
  absl::ReaderMutexLock lock(mu_);
  return last_commit_timestamp_;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
