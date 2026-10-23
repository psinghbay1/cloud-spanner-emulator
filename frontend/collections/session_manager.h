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

#ifndef STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_
#define STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_

#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "common/clock.h"
#include "frontend/collections/multiplexed_session_transaction_manager.h"
#include "frontend/common/labels.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Session manager manages the set of active sessions in the emulator.
class SessionManager {
 public:
  explicit SessionManager(Clock* clock) : clock_(clock) {}

  // Creates a session attached to the given database.
  absl::StatusOr<std::shared_ptr<Session>> CreateSession(
      const Labels& labels, bool multiplexed,
      std::shared_ptr<Database> database,
      MultiplexedSessionTransactionManager* mux_txn_manager)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Returns a session with the given URI.
  absl::StatusOr<std::shared_ptr<Session>> GetSession(
      const std::string& session_uri) ABSL_LOCKS_EXCLUDED(mu_);

  // Deletes a session with the given URI.
  absl::Status DeleteSession(const std::string& session_uri,
                             bool delete_multiplex_sessions = false)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Lists sessions attached to the given database URI.
  absl::StatusOr<std::vector<std::shared_ptr<Session>>> ListSessions(
      const std::string& database_uri,
      bool include_multiplex_sessions = false) const ABSL_LOCKS_EXCLUDED(mu_);

  bool IsMultiplexedSession(const std::string& session_uri) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Erases sessions not used since `not_used_since`, returning how many were
  // removed. Multiplexed sessions are left alone unless `prune_multiplexed`:
  // they expire after 28 days, not an hour, and a client keeps using one.
  //
  // Expiry is otherwise lazy: GetSession() erases an expired session only when
  // it is looked up again, and ListSessions() filters expired sessions out of
  // its response while leaving them in the map, so an abandoned session and its
  // transactions stay in memory for the life of the process.
  int64_t PruneSessionsNotUsedSince(absl::Time not_used_since,
                                    bool prune_multiplexed = false)
      ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // Sweeps idle sessions when --session_idle_timeout is set, at most once per
  // sweep interval so session creation stays cheap.
  void MaybeSweepIdleSessions() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  int64_t PruneSessionsNotUsedSinceLocked(absl::Time not_used_since,
                                          bool prune_multiplexed)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // System-wide clock.
  Clock* clock_;

  // Mutex to guard state below.
  mutable absl::Mutex mu_;

  // Counter for session ids.
  int next_session_id_ ABSL_GUARDED_BY(mu_) = 0;

  // When MaybeSweepIdleSessions() last walked the map.
  absl::Time last_idle_sweep_time_ ABSL_GUARDED_BY(mu_) =
      absl::InfinitePast();

  // Map from session URI to session objects.
  std::map<std::string, std::shared_ptr<Session>> session_map_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_
