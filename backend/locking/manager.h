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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_

#include <memory>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/handle.h"
#include "backend/locking/request.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// LockManager represents the lock manager for a database.
//
// Transactions interact with the LockManager via a LockHandle which they obtain
// at initialization time. All subsequent communication with the LockManager
// happens via the LockHandle. See LockHandle methods for more details about
// this interaction.
//
// Locks are taken on key ranges. Two transactions conflict only when they touch
// the same table, their ranges overlap, and at least one holds the range
// exclusively -- so transactions on unrelated rows proceed concurrently, as
// they do in production Spanner.
//
// Key-range locking is off by default (--enable_key_range_locking=false): the
// manager then holds one whole-database lock, so only one read-write
// transaction runs at a time.
class LockManager {
 public:
  explicit LockManager(Clock* clock) : clock_(clock) {}

  // Returns a handle for a single transaction with the given id and priority.
  // Subsequent communication between the transaction and the lock manager
  // happens via the handle. See LockHandle methods for more details.
  std::unique_ptr<LockHandle> CreateHandle(
      TransactionID id, const std::function<absl::Status()>& abort_fn,
      TransactionPriority priority);

  // Returns the timestamp at which last schema update or commit completed.
  absl::Time LastCommitTimestamp();

 private:
  // LockHandle simply forwards requests to the LockManager.
  friend class LockHandle;
  void EnqueueLock(LockHandle* handle, const LockRequest& request)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UnlockAll(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<absl::Time> ReserveCommitTimestamp(LockHandle* handle)
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status MarkCommitted(LockHandle* handle) ABSL_LOCKS_EXCLUDED(mu_);
  void WaitForSafeRead(absl::Time read_time) ABSL_LOCKS_EXCLUDED(mu_);

  // One entry per range a transaction holds. A vector is adequate: the emulator
  // runs a handful of concurrent transactions, so a linear scan beats the
  // complexity of an interval tree.
  struct HeldLock {
    TransactionID tid;
    LockHandle* handle;
    TableID table_id;
    KeyRange key_range;
    LockMode mode;
  };

  // Returns every DISTINCT transaction holding a range that conflicts with the
  // request, or an empty vector when it can be granted. All of them, not the
  // first: preempting one shared holder and granting an exclusive would leave
  // the other shared holders co-resident with it. Caller must hold mu_.
  std::vector<LockHandle*> FindConflicts(LockHandle* handle,
                                         const LockRequest& request)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Records a granted lock, collapsing an exact repeat. Caller must hold mu_.
  void RecordLock(LockHandle* handle, const LockRequest& request)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Drops every range held by tid. Caller must hold mu_.
  void ReleaseLocksFor(TransactionID tid) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Grants the request by aborting every blocker, or aborts the requester.
  // Caller must hold mu_.
  void ResolveConflict(LockHandle* handle, const LockRequest& request,
                       const std::vector<LockHandle*>& blockers)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Under --lock_conflict_policy=wound_wait: the older transaction wins.
  // Caller must hold mu_.
  void ResolveConflictByWoundWait(LockHandle* handle,
                                  const LockRequest& request,
                                  const std::vector<LockHandle*>& blockers)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Aborts every blocker and grants the request, or returns false if one
  // cannot be aborted. wound_wait selects the error the blockers see. Caller
  // must hold mu_.
  bool WoundAll(LockHandle* handle, const LockRequest& request,
                const std::vector<LockHandle*>& blockers, bool wound_wait)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // True when lhs began first: lower priority (begin time, kept across
  // retries), then lower transaction id.
  static bool IsOlder(LockHandle* lhs, LockHandle* rhs);

  // The pre-flag whole-database lock. Caller must hold mu_.
  void EnqueueDatabaseLock(LockHandle* handle)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Mutex to guard state below.
  absl::Mutex mu_;

  // Ranges currently held, across all transactions.
  std::vector<HeldLock> held_locks_ ABSL_GUARDED_BY(mu_);

  // With key-range locking, the transaction whose commit timestamp is being
  // reserved: commit ordering stays serialised even though lock acquisition is
  // not. Without it, the one transaction holding the whole-database lock.
  LockHandle* active_handle_ ABSL_GUARDED_BY(mu_) = nullptr;

  // System wide monotonic clock used to provide commit and read timestamps.
  Clock* clock_;

  // Timestamp at which last schema update or commit completed.
  absl::Time last_commit_timestamp_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();

  // Commit timestamp being used by an in-progress commit.
  absl::Time pending_commit_timestamp_ ABSL_GUARDED_BY(mu_) =
      absl::InfiniteFuture();

  // Signals completion of a pending commit, and release of the commit-timestamp
  // reservation (active_handle_ cleared) so a waiting committer can proceed.
  absl::CondVar pending_commit_cvar_ ABSL_GUARDED_BY(mu_);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_
