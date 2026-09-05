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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_H_

#include <memory>
#include <string>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/type.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "backend/actions/manager.h"
#include "backend/common/ids.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/pg_oid_assigner/pg_oid_assigner.h"
#include "backend/locking/manager.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/storage/storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace database_api = ::google::spanner::admin::database::v1;

// Database represents a database in the emulator backend.
//
// Database largely ties together various subsystems - transactions, locking,
// schemas, queries, storage etc. and acts as a container for these subsystems.
class Database {
 public:
  // Constructs a fully initialized database with schema created using
  // create_statements. Returns an error if create_statements are invalid, or if
  // failed to create the database.
  static absl::StatusOr<std::unique_ptr<Database>> Create(
      Clock* clock, std::string_view database_id,
      const SchemaChangeOperation& schema_change_operation);

  // Creates a read only transaction attached to this database.
  absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>
  CreateReadOnlyTransaction(const ReadOnlyOptions& options);

  // Creates a read write transaction attached to this database.
  absl::StatusOr<std::unique_ptr<ReadWriteTransaction>>
  CreateReadWriteTransaction(const ReadWriteOptions& options,
                             const RetryState& retry_state);

  // Updates the schema for this database.
  //
  // All schema changes are applied synchronously and transactionally.
  // If there are any transactions or other schema changes already in progress,
  // incoming schema change requests will be rejected with a FAILED_PRECONDITION
  // error.
  //
  // DDL statements in `schema_change_operation.statements` are applied
  // one-by-one until they either all succeed or the first failure is
  // encoutered.
  //
  // On return `num_successful_statements` will contain the number of
  // successfully applied DDL statements and `commit_timestamp` will contain the
  // timestamp at which they were applied.
  //
  // If all the statements in `schema_change_operation.statements` are applied
  // succesfully, both `backfill_status` and the returned status will be set to
  // absl::OkStatus().
  //
  // If all the statements are semantically valid then the return status will
  // be absl::OkStatus(). Otherwise, a non-ok absl::Status will be returned for
  // the first statement that is found to be invalid. In case of a non-ok
  // return status the output parameters (including the `backfill_status`)
  // may not be set.
  //
  // If all the statements are found to be semantically valid, but an error is
  // encountered while processing the backfill/verification actions for the
  // statements, then the first such error will be returned in
  // `backfill_status`.
  absl::Status UpdateSchema(
      const SchemaChangeOperation& schema_change_operation,
      int* num_succesful_statements, absl::Time* commit_timestamp,
      absl::Status* backfill_status);

  // Retrives the current version of the schema.
  const Schema* GetLatestSchema() const;

  // Used to execute queries against the database.
  QueryEngine* query_engine() { return query_engine_.get(); }

  // Returns the database dialect.
  database_api::DatabaseDialect dialect() { return dialect_; }

  ChangeStreamPartitionChurner* get_change_stream_partition_churner() {
    return change_stream_partition_churner_.get();
  }

  PgOidAssigner* get_pg_oid_assigner() { return pg_oid_assigner_.get(); }

  // Counts returned by ReclaimStorage().
  struct ReclaimStats {
    int64_t rows_purged = 0;
    int64_t versions_purged = 0;
    // Live rows erased by the opt-in destructive sweep; 0 unless requested.
    int64_t rows_deleted = 0;
  };

  // Reclaims storage held by deleted rows, dropped tables and dropped columns,
  // then asks the allocator to return free pages to the OS.
  //
  // EMULATOR ONLY. Cloud Spanner has no equivalent API; it reclaims storage in
  // the background. The emulator does not, so a long-lived container otherwise
  // grows for its whole lifetime. Intended for local test harnesses that want
  // to reclaim between test batches instead of restarting the container.
  //
  // `table_names` scopes the sweep to those tables; an empty vector sweeps the
  // whole database. Unknown names are ignored rather than rejected, so a caller
  // sweeping a fixed list need not track schema changes. Idempotent.
  //
  // `window` bounds which versions are eligible. Its `not_before` protects
  // data seeded before a run: seed data is the oldest in the database, so an
  // unbounded sweep reaches it first. Its `not_after` overrides the retention
  // period, letting a caller hold back the most recent writes without the
  // schema change that shortening version_retention_period requires.
  //
  // `delete_rows_in_window` additionally erases LIVE rows whose commit
  // timestamp falls inside `window`. Off by default: it destroys data the
  // application can still read, and is only sound for a local harness
  // clearing test data between batches.
  absl::StatusOr<ReclaimStats> ReclaimStorage(
      const std::vector<std::string>& table_names,
      const PurgeWindow& window = {}, bool delete_rows_in_window = false);

 private:
  Database();
  // Delete copy and assignment operators since database shouldn't be copyable.
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  SchemaChangeContext GetSchemaChangeContext();

  // Clock to provide commit timestamps.
  Clock* clock_;

  // Holds the database id.
  std::string database_id_;

  // Unique ID generator for TransactionID.
  TransactionIDGenerator transaction_id_generator_;

  // Unique ID generator for storage TableIDs.
  TableIDGenerator table_id_generator_;

  // Unique ID generator for storage ChangeStreamIDs.
  ChangeStreamIDGenerator change_stream_id_generator_;

  // Unique ID generator for storage ColumnIDs.
  ColumnIDGenerator column_id_generator_;

  // Underlying storage for the database.
  std::unique_ptr<Storage> storage_;

  // Lock management.
  std::unique_ptr<LockManager> lock_manager_;

  // Type factory used for all GoogleSQL operations on this database.
  std::unique_ptr<googlesql::TypeFactory> type_factory_;

  // Versioned catalog of this database.
  std::unique_ptr<VersionedCatalog> versioned_catalog_;

  // Query engine of the database.
  std::unique_ptr<QueryEngine> query_engine_;

  // Maintains an action registry per schema.
  std::unique_ptr<ActionManager> action_manager_;

  // The database dialect.
  database_api::DatabaseDialect dialect_;

  std::unique_ptr<ChangeStreamPartitionChurner>
      change_stream_partition_churner_;

  // Assigns OIDs to database objects when dialect is POSTGRESQL.
  std::unique_ptr<PgOidAssigner> pg_oid_assigner_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_H_
