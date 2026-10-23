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
#include "absl/time/time.h"

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
    absl::Duration, session_idle_timeout, absl::InfiniteDuration(),
    "If set, regular (non-multiplexed) sessions unused for longer than this "
    "are erased by a sweep that runs as new sessions are created, releasing "
    "sessions that clients abandoned without calling DeleteSession. A later "
    "request on a swept session fails with NOT_FOUND, as Cloud Spanner does "
    "for an expired session. Unset (the default) never sweeps.");

ABSL_FLAG(
    absl::Duration, operation_retention, absl::InfiniteDuration(),
    "If set, finished long-running operations (CreateDatabase, UpdateDdl, ...) "
    "are erased this long after they complete, by a sweep that runs as new "
    "operations are created. A swept operation can no longer be polled and "
    "GetOperation returns NOT_FOUND. In-flight operations are never erased. "
    "Unset (the default) keeps operations until DeleteOperation is called.");

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

absl::Duration session_idle_timeout() {
  return absl::GetFlag(FLAGS_session_idle_timeout);
}

void set_session_idle_timeout(absl::Duration timeout) {
  absl::SetFlag(&FLAGS_session_idle_timeout, timeout);
}

absl::Duration operation_retention() {
  return absl::GetFlag(FLAGS_operation_retention);
}

void set_operation_retention(absl::Duration retention) {
  absl::SetFlag(&FLAGS_operation_retention, retention);
}

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google
