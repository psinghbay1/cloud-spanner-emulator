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

#include "common/config.h"

#include <string>

#include "absl/flags/flag.h"

ABSL_FLAG(std::string, host_port, "localhost:10007",
          "Emulator host IP and port that serves Cloud Spanner gRPC requests.");

ABSL_FLAG(bool, log_requests, false,
          "If true, gRPC request and response messages are streamed to the "
          "INFO log. This switch is intended for emulator debugging.");

ABSL_FLAG(
    bool, enable_fault_injection, false,
    "If true, the emulator will inject faults to allow testing application "
    "error handling behavior. For instance, transaction Commits may be aborted "
    "to facilitate application abort-retry testing.");

ABSL_FLAG(bool, disable_query_null_filtered_index_check, false,
          "If true, then queries that use NULL_FILTERED indexes will be "
          "answered. Please test all queries using null filtered indexes "
          "against production Cloud Spanner before disabling this check."
          "\n"
          "Please consider using the query hint "
          "`@{spanner_emulator.disable_query_null_filtered_index_check=true}` "
          "to disable this check per query, instead of disabling this check "
          "for all the queries at once.");

ABSL_FLAG(
    int, abort_current_transaction_probability, 20,
    "The probability that the emulator will try to abort the current "
    "transaction if a new transaction is requested. A higher value gives "
    "higher priority to new transactions. A lower value gives higher priority "
    "to the current transaction. A value of zero means that the emulator will "
    "never abort the current transaction.");

ABSL_FLAG(
    bool, enable_key_range_locking, false,
    "If true, read-write transactions lock key ranges per table (shared for "
    "reads, exclusive for writes) instead of the whole database, so "
    "transactions that touch disjoint keys run concurrently. Conflicting "
    "transactions are still aborted with ABORTED. If false (the default), only "
    "one read-write transaction runs at a time.");

ABSL_FLAG(
    std::string, lock_conflict_policy, "random",
    "With --enable_key_range_locking, how a lock conflict is resolved. "
    "'random' (the default) aborts the holder with "
    "--abort_current_transaction_probability, else the requester. "
    "'wound_wait' decides by transaction age, as Cloud Spanner does: an older "
    "requester aborts (wounds) a younger holder, and a younger requester is "
    "aborted. Any other value is treated as 'random'. Ignored when key-range "
    "locking is disabled.");

ABSL_FLAG(
    int, commit_reservation_wait_ms, 5000,
    "With --enable_key_range_locking, how long in milliseconds a committing "
    "transaction waits for another transaction to finish reserving its commit "
    "timestamp before it is aborted. 0 aborts at once. Ignored when key-range "
    "locking is disabled.");

namespace google {
namespace spanner {
namespace emulator {
namespace config {

std::string grpc_host_port() { return absl::GetFlag(FLAGS_host_port); }

bool should_log_requests() { return absl::GetFlag(FLAGS_log_requests); }

bool fault_injection_enabled() {
  return absl::GetFlag(FLAGS_enable_fault_injection);
}

bool disable_query_null_filtered_index_check() {
  return absl::GetFlag(FLAGS_disable_query_null_filtered_index_check);
}

int abort_current_transaction_probability() {
  return absl::GetFlag(FLAGS_abort_current_transaction_probability);
}

void set_abort_current_transaction_probability(int probability) {
  absl::SetFlag(&FLAGS_abort_current_transaction_probability, probability);
}

bool enable_key_range_locking() {
  return absl::GetFlag(FLAGS_enable_key_range_locking);
}

void set_enable_key_range_locking(bool enabled) {
  absl::SetFlag(&FLAGS_enable_key_range_locking, enabled);
}

LockConflictPolicy lock_conflict_policy() {
  if (absl::GetFlag(FLAGS_lock_conflict_policy) == "wound_wait") {
    return LockConflictPolicy::kWoundWait;
  }
  return LockConflictPolicy::kRandom;
}

void set_lock_conflict_policy(LockConflictPolicy policy) {
  absl::SetFlag(&FLAGS_lock_conflict_policy,
                policy == LockConflictPolicy::kWoundWait ? "wound_wait"
                                                         : "random");
}

int commit_reservation_wait_ms() {
  return absl::GetFlag(FLAGS_commit_reservation_wait_ms);
}

void set_commit_reservation_wait_ms(int wait_ms) {
  absl::SetFlag(&FLAGS_commit_reservation_wait_ms, wait_ms);
}

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google
