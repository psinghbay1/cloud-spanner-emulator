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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CONFIG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CONFIG_H_

#include <string>

#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace config {

// The address at which the emulator will serve gRPC requests.
std::string grpc_host_port();

// If true, gRPC requests and response messages are streamed to the INFO log.
bool should_log_requests();

// Returns true if fault injection is enabled.
bool fault_injection_enabled();

// If true, then queries that use NULL_FILTERED indexes will be answered.
//
// Note: The emulator cannot be used to validate the behavior of NULL_FILTERED
// indexes. Before disabling this check, please ensure that you have tested the
// queries that use null filtered indexes against production Cloud Spanner.
//
// Please consider using the query hint
// `@{spanner_emulator.disable_query_null_filtered_index_check=true}` to disable
// this check per query, instead of disabling this check for all the queries at
// once.
bool disable_query_null_filtered_index_check();

// The probability that the emulator will try to abort the current transaction
// if a new transaction is requested. A higher value gives higher priority to
// new transactions. A lower value gives higher priority to the current
// transaction. A value of zero means that the emulator will never abort the
// current transaction.
int abort_current_transaction_probability();

void set_abort_current_transaction_probability(int probability);

// True when the lock manager locks key ranges per table rather than the whole
// database, so read-write transactions on disjoint keys run concurrently.
// Off by default.
bool enable_key_range_locking();
void set_enable_key_range_locking(bool enabled);

// With key-range locking, how long, in milliseconds, a committing transaction
// waits for a peer's commit-timestamp reservation to clear before it is
// aborted. 0 means abort at once. Ignored when key-range locking is off.
int commit_reservation_wait_ms();
void set_commit_reservation_wait_ms(int wait_ms);

// Interval of the background sweep that erases deleted rows and superseded
// versions older than the version retention period. A non-positive or infinite
// value disables the sweep, which is the default.
absl::Duration storage_purge_interval();

void set_storage_purge_interval(absl::Duration interval);

// True when ExecuteSql and ExecuteStreamingSql answer the emulator-only
// statements SELECT EMULATOR_RECLAIM(...) and SELECT EMULATOR_LOCK_STATS().
// Off by default, so they fail as unknown functions, as on Cloud Spanner.
bool enable_emulator_diagnostic_statements();
void set_enable_emulator_diagnostic_statements(bool enabled);

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_FLAGS_H_
