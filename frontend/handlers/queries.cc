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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/spanner/v1/keys.pb.h"
#include "google/spanner/v1/query_plan.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "googlesql/public/analyzer_options.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"
#include "backend/access/read.h"
#include "backend/database/database.h"
#include "backend/storage/storage.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/collections/session_manager.h"
#include "backend/query/change_stream/change_stream_query_validator.h"
#include "backend/query/query_engine.h"
#include "common/constants.h"
#include "common/errors.h"
#include "frontend/common/protos.h"
#include "frontend/common/validations.h"
#include "frontend/converters/partition.h"
#include "frontend/converters/query.h"
#include "frontend/converters/reads.h"
#include "frontend/converters/types.h"
#include "frontend/converters/values.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"
#include "frontend/entities/transaction.h"
#include "frontend/handlers/change_streams.h"
#include "frontend/proto/partition_token.pb.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "googlesql/base/status_macros.h"
#include "farmhash.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace spanner_api = ::google::spanner::v1;

namespace {

absl::Duration kMaxFutureReadDuration = absl::Hours(1);

absl::Status ValidateReadTimestampNotTooFarInFuture(absl::Time read_timestamp,
                                                    absl::Time now) {
  if (read_timestamp - now > kMaxFutureReadDuration) {
    return error::ReadTimestampTooFarInFuture(read_timestamp);
  }
  return absl::OkStatus();
}

absl::Status ValidateTransactionSelectorForQuery(
    const spanner_api::TransactionSelector& selector, bool is_dml) {
  if (selector.selector_case() ==
          spanner_api::TransactionSelector::SelectorCase::kSingleUse &&
      selector.single_use().mode_case() != v1::TransactionOptions::kReadOnly) {
    return error::InvalidModeForReadOnlySingleUseTransaction();
  }
  if (is_dml) {
    if (selector.begin().mode_case() == v1::TransactionOptions::kReadOnly) {
      // ReadWrite and PartitionedDML transactions are currently allowed.
      return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
    }
    if (selector.selector_case() ==
        spanner_api::TransactionSelector::SelectorCase::kSingleUse) {
      return error::DmlDoesNotSupportSingleUseTransaction();
    }
  }
  return absl::OkStatus();
}

absl::Status ValidatePartitionToken(
    const PartitionToken& partition_token,
    const spanner_api::ExecuteSqlRequest* request) {
  if (request->query_mode() != v1::ExecuteSqlRequest::NORMAL) {
    return error::InvalidPartitionedQueryMode();
  }
  if (partition_token.session() != request->session()) {
    return error::ReadFromDifferentSession();
  }
  if (request->transaction().selector_case() != v1::TransactionSelector::kId ||
      partition_token.transaction_id() != request->transaction().id()) {
    return error::ReadFromDifferentTransaction();
  }

  if (!partition_token.has_query_params()) {
    return error::ReadFromDifferentParameters();
  }
  auto query_params = partition_token.query_params();

  if (query_params.sql() != request->sql()) {
    return error::ReadFromDifferentParameters();
  }

  if (query_params.params().fields_size() != request->params().fields_size()) {
    return error::ReadFromDifferentParameters();
  }
  for (const auto& field : query_params.params().fields()) {
    if (!request->params().fields().contains(field.first) ||
        field.second.SerializeAsString() !=
            request->params().fields().at(field.first).SerializeAsString()) {
      return error::ReadFromDifferentParameters();
    }
  }

  if (query_params.param_types_size() != request->param_types_size()) {
    return error::ReadFromDifferentParameters();
  }
  for (const auto& param_type : query_params.param_types()) {
    if (!request->param_types().contains(param_type.first) ||
        param_type.second.GetTypeName() !=
            request->param_types().at(param_type.first).GetTypeName()) {
      return error::ReadFromDifferentParameters();
    }
  }

  return absl::OkStatus();
}

void AddQueryStatsFromQueryResult(const backend::QueryResult& result,
                                  google::protobuf::Struct* stats) {
  (*stats->mutable_fields())["rows_returned"].set_string_value(
      absl::StrCat(result.num_output_rows));
  (*stats->mutable_fields())["elapsed_time"].set_string_value(
      absl::FormatDuration(result.elapsed_time));
}

void AddEmptyQueryPlan(v1::ResultSetStats* stats) {
  auto node = stats->mutable_query_plan()->add_plan_nodes();
  auto display_name = std::string("No query plan");
  node->mutable_display_name()->assign(display_name);
  node->set_kind(v1::PlanNode::KIND_UNSPECIFIED);
}

absl::Status AddUndeclaredParametersFromQueryResult(
    googlesql::QueryParametersMap* cursor, v1::ResultSetMetadata* metadata_pb) {
  for (auto const& param : *cursor) {
    auto* field_pb = metadata_pb->mutable_undeclared_parameters()->add_fields();
    field_pb->set_name(param.first);
    GOOGLESQL_RETURN_IF_ERROR(TypeToProto(param.second, field_pb->mutable_type()))
        << " when converting param " << param.first << " of type "
        << param.second << " in row cursor";
  }
  return absl::OkStatus();
}

absl::StatusOr<backend::QueryResult> ExecuteQuery(
    const spanner_api::ExecuteBatchDmlRequest_Statement& statement,
    std::shared_ptr<Transaction> txn,
    const google::protobuf::Map<std::string, google::protobuf::Value>& secure_context) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const backend::Query query,
      QueryFromProto(statement.sql(), statement.params(),
                     statement.param_types(),
                     txn->query_engine()->type_factory(),
                     txn->schema()->proto_bundle(), secure_context));
  return txn->ExecuteSql(query);
}

template <typename Request>
int64_t SerializeAndHashRequest(const Request& request) {
  std::string serialized_request;
  {
    // Serialize the request proto deterministically.
    // Message::SerializeToString() is not guaranteed to deterministically
    // generate the same string for a message that contains map fields.
    // We create the output stream in an inner scope so that it gets flushed
    // in the destructor before computing the fingerprint.
    google::protobuf::io::StringOutputStream stream(&serialized_request);
    google::protobuf::io::CodedOutputStream output(&stream);
    output.SetSerializationDeterministic(true);
    request.SerializeToCodedStream(&output);
  }
  return farmhash::Fingerprint64(serialized_request);
}

int64_t HashRequest(const spanner_api::ExecuteSqlRequest* request) {
  spanner_api::ExecuteSqlRequest copy = *request;
  // Clearing resume token and sequence number so that the hash is based
  // entirely on the sql statement.
  copy.clear_resume_token();
  copy.set_seqno(0);
  return SerializeAndHashRequest(copy);
}

int64_t HashRequest(const spanner_api::ExecuteBatchDmlRequest* request) {
  spanner_api::ExecuteBatchDmlRequest copy = *request;
  // Clearing sequence number so that the hash is based entirely on the sql
  // statement.
  copy.set_seqno(0);
  return SerializeAndHashRequest(copy);
}

// Emulator-only escape hatch: SELECT EMULATOR_RECLAIM('T1', 'T2')
//
// Cloud Spanner reclaims storage in the background; the emulator does not, so
// a long-lived container grows for its whole lifetime. This lets a local test
// harness reclaim between batches instead of restarting the container.
//
// Recognised only as a whole statement, so it cannot collide with a real query
// or a column named similarly. With no arguments it sweeps the whole database.
constexpr char kReclaimPrefix[] = "SELECT EMULATOR_RECLAIM(";

bool IsEmulatorReclaimStatement(absl::string_view sql) {
  return absl::StartsWithIgnoreCase(absl::StripAsciiWhitespace(sql),
                                    kReclaimPrefix);
}

// Splits the argument list into its comma-separated arguments, ignoring commas
// inside quotes. Returns the text between the outermost parentheses.
std::vector<std::string> SplitReclaimArguments(absl::string_view sql) {
  absl::string_view rest = absl::StripAsciiWhitespace(sql);
  rest.remove_prefix(sizeof(kReclaimPrefix) - 1);
  size_t close_paren = rest.rfind(')');
  if (close_paren != absl::string_view::npos) {
    rest = rest.substr(0, close_paren);
  }

  std::vector<std::string> arguments;
  bool in_quotes = false;
  size_t start = 0;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '\'') {
      in_quotes = !in_quotes;
    } else if (rest[i] == ',' && !in_quotes) {
      arguments.emplace_back(absl::StripAsciiWhitespace(
          rest.substr(start, i - start)));
      start = i + 1;
    }
  }
  absl::string_view last = absl::StripAsciiWhitespace(rest.substr(start));
  if (!last.empty()) arguments.emplace_back(last);
  return arguments;
}

// True for a named argument of the form `name => value`.
bool IsNamedArgument(absl::string_view argument, absl::string_view name) {
  size_t arrow = argument.find("=>");
  if (arrow == absl::string_view::npos) return false;
  return absl::EqualsIgnoreCase(
      absl::StripAsciiWhitespace(argument.substr(0, arrow)), name);
}

// The value of a named argument, with its surrounding quotes removed.
std::string NamedArgumentValue(absl::string_view argument) {
  absl::string_view value =
      absl::StripAsciiWhitespace(argument.substr(argument.find("=>") + 2));
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    value = value.substr(1, value.size() - 2);
  }
  return std::string(value);
}

// Extracts the single-quoted table names from the argument list, skipping the
// named arguments. Returns an empty vector for EMULATOR_RECLAIM(), meaning
// "the whole database".
std::vector<std::string> ParseReclaimTableNames(absl::string_view sql) {
  std::vector<std::string> table_names;
  for (const std::string& argument : SplitReclaimArguments(sql)) {
    if (argument.find("=>") != std::string::npos) continue;
    absl::string_view name = argument;
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
      name = name.substr(1, name.size() - 2);
    }
    if (!name.empty()) table_names.emplace_back(name);
  }
  return table_names;
}

// Reads the optional window bounds:
//
//   not_before    => '2026-09-03T19:00:00Z'   protect everything seeded earlier
//   not_after     => '2026-09-03T20:00:00Z'   absolute upper bound
//   not_after_lag => '60s'                    upper bound relative to now
//
// not_after_lag is the useful form for a long run: it keeps the newest minute
// of writes safe no matter how long the suite takes. Absent bounds leave the
// retention behaviour untouched.
// True when the caller passed delete_rows => true, opting in to erasing live
// rows whose commit timestamp falls inside the window.
bool ParseReclaimDeleteRows(absl::string_view sql) {
  for (const std::string& argument : SplitReclaimArguments(sql)) {
    if (IsNamedArgument(argument, "delete_rows") &&
        absl::EqualsIgnoreCase(NamedArgumentValue(argument), "true")) {
      return true;
    }
  }
  return false;
}

// True when the caller passed prune_sessions => true.
bool ParseReclaimPruneSessions(absl::string_view sql) {
  for (const std::string& argument : SplitReclaimArguments(sql)) {
    if (IsNamedArgument(argument, "prune_sessions") &&
        absl::EqualsIgnoreCase(NamedArgumentValue(argument), "true")) {
      return true;
    }
  }
  return false;
}

// True when the caller passed prune_operations => true.
bool ParseReclaimPruneOperations(absl::string_view sql) {
  for (const std::string& argument : SplitReclaimArguments(sql)) {
    if (IsNamedArgument(argument, "prune_operations") &&
        absl::EqualsIgnoreCase(NamedArgumentValue(argument), "true")) {
      return true;
    }
  }
  return false;
}

absl::StatusOr<backend::PurgeWindow> ParseReclaimWindow(absl::string_view sql,
                                                        absl::Time now) {
  backend::PurgeWindow window;
  for (const std::string& argument : SplitReclaimArguments(sql)) {
    absl::Time parsed;
    std::string error;
    if (IsNamedArgument(argument, "not_before") ||
        IsNamedArgument(argument, "not_after")) {
      const std::string value = NamedArgumentValue(argument);
      if (!absl::ParseTime(absl::RFC3339_full, value, &parsed, &error)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "EMULATOR_RECLAIM: could not parse timestamp \"", value,
            "\": ", error));
      }
      if (IsNamedArgument(argument, "not_before")) {
        window.not_before = parsed;
      } else {
        window.not_after = parsed;
      }
    } else if (IsNamedArgument(argument, "not_after_lag")) {
      const std::string value = NamedArgumentValue(argument);
      absl::Duration lag;
      if (!absl::ParseDuration(value, &lag)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "EMULATOR_RECLAIM: could not parse duration \"", value, "\""));
      }
      window.not_after = now - lag;
    }
  }
  if (window.not_before.has_value() && window.not_after.has_value() &&
      *window.not_before >= *window.not_after) {
    return absl::InvalidArgumentError(
        "EMULATOR_RECLAIM: not_before must be earlier than not_after");
  }
  return window;
}

// Answers the reclaim statement with a one-row, one-column INT64 result set
// holding the number of purged rows.
absl::Status HandleEmulatorReclaim(RequestContext* ctx,
                                   std::shared_ptr<Session> session,
                                   absl::string_view sql,
                                   spanner_api::ResultSet* response) {
  // Session::database() hands back the frontend wrapper; ReclaimStorage lives on
  // the backend database it holds.
  // The statement verbatim, so the log shows what was asked for even if an
  // argument was mistyped and silently ignored.
  ABSL_LOG(INFO) << "[reclaim] statement: " << sql;

  GOOGLESQL_ASSIGN_OR_RETURN(backend::PurgeWindow window,
                   ParseReclaimWindow(sql, absl::Now()));
  const bool delete_rows = ParseReclaimDeleteRows(sql);
  // An unbounded destructive sweep would erase every live row in scope, which
  // is never what a harness means. Require a bound rather than doing it.
  if (delete_rows && !window.not_before.has_value() &&
      !window.not_after.has_value()) {
    return absl::InvalidArgumentError(
        "EMULATOR_RECLAIM: delete_rows requires not_before, not_after or "
        "not_after_lag -- refusing to erase every live row");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(backend::Database::ReclaimStats stats,
                   session->database()->backend()->ReclaimStorage(
                       ParseReclaimTableNames(sql), window, delete_rows));

  // Sessions are frontend state, so they are swept here rather than in the
  // backend. A session not used since the window's upper bound cannot be in
  // use by a running test, and an already-expired session is unusable anyway --
  // GetSession() reports it as not found -- so this only frees dead memory.
  int64_t sessions_pruned = 0;
  if (ParseReclaimPruneSessions(sql)) {
    const absl::Time not_used_since =
        window.not_after.value_or(absl::Now() - absl::Hours(1));
    sessions_pruned =
        ctx->env()->session_manager()->PruneSessionsNotUsedSince(
            not_used_since);
    // Logged in the container so a harness run can be diagnosed from the log
    // alone; the CLI's own output is gone once the caller exits.
    ABSL_LOG(INFO) << "[reclaim] prune_sessions not_used_since="
                   << absl::FormatTime(absl::RFC3339_full, not_used_since,
                                       absl::UTCTimeZone())
                   << ", sessions_pruned=" << sessions_pruned;
  }

  // Operations are created per CreateDatabase and UpdateDdl and removed only by
  // an explicit DeleteOperation, so schema churn accumulates them. Only
  // completed ones are pruned; there is no window, because a completed
  // operation is finished work regardless of when it ran.
  int64_t operations_pruned = 0;
  if (ParseReclaimPruneOperations(sql)) {
    operations_pruned =
        ctx->env()->operation_manager()->PruneCompletedOperations();
    ABSL_LOG(INFO) << "[reclaim] prune_operations completed_only=true"
                   << ", operations_pruned=" << operations_pruned;
  }

  auto* row_type = response->mutable_metadata()->mutable_row_type();
  auto* rows_field = row_type->add_fields();
  rows_field->set_name("rows_purged");
  rows_field->mutable_type()->set_code(google::spanner::v1::TypeCode::INT64);
  auto* versions_field = row_type->add_fields();
  versions_field->set_name("versions_purged");
  versions_field->mutable_type()->set_code(google::spanner::v1::TypeCode::INT64);
  auto* deleted_field = row_type->add_fields();
  deleted_field->set_name("rows_deleted");
  deleted_field->mutable_type()->set_code(google::spanner::v1::TypeCode::INT64);
  auto* sessions_field = row_type->add_fields();
  sessions_field->set_name("sessions_pruned");
  sessions_field->mutable_type()->set_code(google::spanner::v1::TypeCode::INT64);
  auto* operations_field = row_type->add_fields();
  operations_field->set_name("operations_pruned");
  operations_field->mutable_type()->set_code(
      google::spanner::v1::TypeCode::INT64);

  auto* row = response->add_rows();
  row->add_values()->set_string_value(absl::StrCat(stats.rows_purged));
  row->add_values()->set_string_value(absl::StrCat(stats.versions_purged));
  row->add_values()->set_string_value(absl::StrCat(stats.rows_deleted));
  row->add_values()->set_string_value(absl::StrCat(sessions_pruned));
  row->add_values()->set_string_value(absl::StrCat(operations_pruned));
  return absl::OkStatus();
}

}  //  namespace

// Executes a SQL statement, returning all results in a single reply.
absl::Status ExecuteSql(RequestContext* ctx,
                        const spanner_api::ExecuteSqlRequest* request,
                        spanner_api::ResultSet* response) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Emulator-only storage reclamation. Handled before transaction setup and
  // query planning: it is not a real query and takes no transaction.
  if (IsEmulatorReclaimStatement(request->sql())) {
    return HandleEmulatorReclaim(ctx, session, request->sql(), response);
  }

  // Get underlying transaction.
  bool is_dml_query = backend::IsDMLQuery(request->sql());
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      is_dml_query));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));

  // Wrap all operations on this transaction so they are atomic.
  return txn->GuardedCall(
      is_dml_query ? Transaction::OpType::kDml : Transaction::OpType::kSql,
      [&]() -> absl::Status {
        if (request->data_boost_enabled()) {
          if (request->partition_token().empty()) {
            return error::DataBoostRequiresPartitionToken();
          }
        }
        // Register DML request and check for status replay.
        if (is_dml_query) {
          const auto state = txn->LookupOrRegisterDmlRequest(
              request->seqno(), HashRequest(request), request->sql());
          if (state.has_value()) {
            if (!state->status.ok()) {
              return state->status;
            }
            if (!std::holds_alternative<spanner_api::ResultSet>(
                    state->outcome)) {
              return error::ReplayRequestMismatch(request->seqno(),
                                                  request->sql());
            }
            *response = std::get<spanner_api::ResultSet>(state->outcome);
            return state->status;
          }

          // DML needs to explicitly check the transaction status since
          // the DML sequence number replay should take priority over returning
          // a previously encountered error status.
          GOOGLESQL_RETURN_IF_ERROR(txn->Status());
        }

        // Cannot query after commit, rollback, or non-recoverable error.
        if (txn->IsInvalid()) {
          return error::CannotUseTransactionAfterConstraintError();
        }
        if (txn->IsCommitted() || txn->IsRolledback()) {
          if (txn->IsPartitionedDml()) {
            return error::CannotReusePartitionedDmlTransaction();
          }
          return error::CannotReadOrQueryAfterCommitOrRollback();
        }
        if (txn->IsReadOnly()) {
          if (is_dml_query) {
            return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
          }
          GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
          GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
              read_timestamp, ctx->env()->clock()->Now()));
        }

        // Convert and execute provided SQL statement.
        GOOGLESQL_ASSIGN_OR_RETURN(
            const backend::Query query,
            QueryFromProto(
                request->sql(), request->params(), request->param_types(),
                txn->query_engine()->type_factory(),
                txn->schema()->proto_bundle(),
                request->request_options().client_context().secure_context()));
        auto maybe_result = txn->ExecuteSql(query, request->query_mode());
        if (!maybe_result.ok()) {
          absl::Status error = maybe_result.status();
          if (txn->IsPartitionedDml()) {
            // A Partitioned DML transaction will become invalidated on any
            // error.
            error.SetPayload(kConstraintError, absl::Cord(""));
          }
          if (ShouldReturnTransaction(request->transaction())) {
            // The transaction ID has not been returned to the user yet, so we
            // must rollback the transaction to avoid leaving it in an active
            // state.
            txn->Rollback().IgnoreError();
          }
          return error;
        }
        backend::QueryResult& result = maybe_result.value();

        // Populate transaction metadata.
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(*response->mutable_metadata()->mutable_transaction(),
                           txn->ToProto());
        }

        if (txn->IsReadWrite() && session->multiplexed()) {
          // Set an empty precommit token.
          response->mutable_precommit_token();
        }

        // Return query parameter types.
        GOOGLESQL_RETURN_IF_ERROR(AddUndeclaredParametersFromQueryResult(
            &result.parameter_types, response->mutable_metadata()));

        if (is_dml_query) {
          if (txn->IsPartitionedDml()) {
            response->mutable_stats()->set_row_count_lower_bound(
                result.modified_row_count);
          } else {
            response->mutable_stats()->set_row_count_exact(
                result.modified_row_count);
          }

          if (result.rows == nullptr) {
            // Set empty row type.
            response->mutable_metadata()->mutable_row_type();
          } else {
            // It contains DML THEN RETURN row results.
            GOOGLESQL_RETURN_IF_ERROR(RowCursorToResultSetProto(result.rows.get(),
                                                      /*limit=*/0, response));
          }
        } else {
          GOOGLESQL_RETURN_IF_ERROR(RowCursorToResultSetProto(result.rows.get(),
                                                    /*limit=*/0, response));
        }

        if (!request->partition_token().empty()) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              auto partition_token,
              PartitionTokenFromString(request->partition_token()));
          GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionToken(partition_token, request));
          if (partition_token.empty_query_partition()) {
            response->clear_rows();
          }
        }

        // Add basic stats for PROFILE mode. We do this to interoperate with
        // REPL applications written for Cloud Spanner. The profile will not
        // contain statistics for plan nodes.
        if (request->query_mode() == spanner_api::ExecuteSqlRequest::PROFILE) {
          AddQueryStatsFromQueryResult(
              result, response->mutable_stats()->mutable_query_stats());
        }
        // Add an empty query plan if the user requested either PLAN or PROFILE
        // query mode.
        if (request->query_mode() == spanner_api::ExecuteSqlRequest::PLAN ||
            request->query_mode() == spanner_api::ExecuteSqlRequest::PROFILE) {
          AddEmptyQueryPlan(response->mutable_stats());
        }

        if (is_dml_query) {
          txn->SetDmlReplayOutcome(*response);
        }
        return absl::OkStatus();
      });
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteSql);

// Executes a SQL statement, returning all results as a stream.
//
// resume_tokens is not supported in the emulator. This implementation does not
// limit the size of the response and therefore, chunked_value will always be
// false.
absl::Status ExecuteStreamingSql(
    RequestContext* ctx, const spanner_api::ExecuteSqlRequest* request,
    ServerStream<spanner_api::PartialResultSet>* stream) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  backend::ChangeStreamQueryValidator::ChangeStreamMetadata
      change_stream_metadata;

  // Get underlying transaction.
  bool is_dml_query = backend::IsDMLQuery(request->sql());

  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      is_dml_query));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateDirectedReadsOption(request->directed_read_options(), txn));

  // Wrap all operations on this transaction so they are atomic.
  absl::Status status = txn->GuardedCall(
      is_dml_query ? Transaction::OpType::kDml : Transaction::OpType::kSql,
      [&]() -> absl::Status {
        if (request->data_boost_enabled()) {
          if (request->partition_token().empty()) {
            return error::DataBoostRequiresPartitionToken();
          }
        }
        // Register DML request and check for status replay.
        if (is_dml_query) {
          const auto state = txn->LookupOrRegisterDmlRequest(
              request->seqno(), HashRequest(request), request->sql());
          if (state.has_value()) {
            if (!state->status.ok()) {
              return state->status;
            }
            if (!std::holds_alternative<spanner_api::ResultSet>(
                    state->outcome)) {
              return error::ReplayRequestMismatch(request->seqno(),
                                                  request->sql());
            }
            spanner_api::PartialResultSet response;
            spanner_api::ResultSet replay_result =
                std::get<spanner_api::ResultSet>(state->outcome);
            *response.mutable_stats() = replay_result.stats();
            *response.mutable_metadata() = replay_result.metadata();
            if (session->multiplexed() && txn->IsReadWrite()) {
              response.mutable_precommit_token();
            }
            stream->Send(response);
            return state->status;
          }

          // DML needs to explicitly check the transaction status since
          // the DML sequence number replay should take priority over returning
          // a previously encountered error status.
          GOOGLESQL_RETURN_IF_ERROR(txn->Status());
        }

        // Cannot query after commit, rollback, or non-recoverable error.
        if (txn->IsInvalid()) {
          return error::CannotUseTransactionAfterConstraintError();
        }
        if (txn->IsCommitted() || txn->IsRolledback()) {
          if (txn->IsPartitionedDml()) {
            return error::CannotReusePartitionedDmlTransaction();
          }
          return error::CannotReadOrQueryAfterCommitOrRollback();
        }
        if (txn->IsReadOnly()) {
          if (is_dml_query) {
            return error::ReadOnlyTransactionDoesNotSupportDml("ReadOnly");
          }
          GOOGLESQL_ASSIGN_OR_RETURN(absl::Time read_timestamp, txn->GetReadTimestamp());
          GOOGLESQL_RETURN_IF_ERROR(ValidateReadTimestampNotTooFarInFuture(
              read_timestamp, ctx->env()->clock()->Now()));
        }
        // Convert and execute provided SQL statement.
        GOOGLESQL_ASSIGN_OR_RETURN(
            const backend::Query query,
            QueryFromProto(
                request->sql(), request->params(), request->param_types(),
                txn->query_engine()->type_factory(),
                txn->schema()->proto_bundle(),
                request->request_options().client_context().secure_context()));
        bool in_read_write_txn = txn->IsReadWrite() || txn->IsPartitionedDml();
        GOOGLESQL_ASSIGN_OR_RETURN(change_stream_metadata,
                         backend::QueryEngine::TryGetChangeStreamMetadata(
                             query, txn->schema(), in_read_write_txn));
        // if current query is a change stream query, return and exit current
        // transaction lambda to avoid nested transaction call.
        if (change_stream_metadata.is_change_stream_query) {
          return absl::OkStatus();
        }
        auto maybe_result = txn->ExecuteSql(query, request->query_mode());
        if (!maybe_result.ok()) {
          absl::Status error = maybe_result.status();
          if (txn->IsPartitionedDml()) {
            // A Partitioned DML transaction will become invalidated on any
            // error.
            error.SetPayload(kConstraintError, absl::Cord(""));
          }
          if (ShouldReturnTransaction(request->transaction())) {
            // The transaction ID has not been returned to the user yet, so we
            // must rollback the transaction to avoid leaving it in an active
            // state.
            txn->Rollback().IgnoreError();
          }
          return error;
        }
        backend::QueryResult& result = maybe_result.value();

        std::vector<spanner_api::PartialResultSet> responses;
        if (is_dml_query) {
          responses.emplace_back();
          if (result.rows == nullptr) {
            // Set empty row type.
            responses.back().mutable_metadata()->mutable_row_type();
          } else {
            // It contains DML THEN RETURN row results.
            GOOGLESQL_ASSIGN_OR_RETURN(responses, RowCursorToPartialResultSetProtos(
                                            result.rows.get(), /*limit=*/0));
          }
          if (txn->IsPartitionedDml()) {
            responses.back().mutable_stats()->set_row_count_lower_bound(
                result.modified_row_count);
          } else {
            responses.back().mutable_stats()->set_row_count_exact(
                result.modified_row_count);
          }
        } else {
          GOOGLESQL_ASSIGN_OR_RETURN(responses, RowCursorToPartialResultSetProtos(
                                          result.rows.get(), /*limit=*/0));
        }
        if (session->multiplexed() && txn->IsReadWrite()) {
          for (auto& response : responses) {
            response.mutable_precommit_token();
          }
        }

          if (!request->partition_token().empty()) {
            PartitionToken partition_token;
            GOOGLESQL_ASSIGN_OR_RETURN(
                auto parsed_token,
                PartitionTokenFromString(request->partition_token()));
            partition_token = std::move(parsed_token);
          GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionToken(partition_token, request));
          if (partition_token.empty_query_partition()) {
            // Clear all partial responses except the first one. Return only
            // metadata in the first partial response.
            responses.resize(1);
            responses.front().clear_values();
            responses.front().clear_chunked_value();
          }
        }

        // Populate transaction metadata.
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              *responses.front().mutable_metadata()->mutable_transaction(),
              txn->ToProto());
        }
        // Return query parameter types.
        GOOGLESQL_RETURN_IF_ERROR(AddUndeclaredParametersFromQueryResult(
            &result.parameter_types, responses.front().mutable_metadata()));

        // Add basic stats for PROFILE mode. We do this to interoperate with
        // REPL applications written for Cloud Spanner. The profile will not
        // contain statistics for plan nodes.
        if (request->query_mode() == spanner_api::ExecuteSqlRequest::PROFILE) {
          AddQueryStatsFromQueryResult(
              result, responses.front().mutable_stats()->mutable_query_stats());
        }

        // Send results back to client.
        for (const auto& response : responses) {
          stream->Send(response);
        }

        if (is_dml_query) {
          spanner_api::ResultSet replay_result;
          *replay_result.mutable_stats() = responses[0].stats();
          *replay_result.mutable_metadata() = responses[0].metadata();
          txn->SetDmlReplayOutcome(replay_result);
        }
        return absl::OkStatus();
      });
  if (change_stream_metadata.is_change_stream_query) {
    ChangeStreamsHandler change_streams_handler{change_stream_metadata};
    return change_streams_handler.ExecuteChangeStreamQuery(request, stream,
                                                           session);
  }
  return status;
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteStreamingSql);

// Executes a batch of DML statements.
absl::Status ExecuteBatchDml(RequestContext* ctx,
                             const spanner_api::ExecuteBatchDmlRequest* request,
                             spanner_api::ExecuteBatchDmlResponse* response) {
  // Verify the request has DML statement(s).
  if (request->statements().empty()) {
    return error::InvalidBatchDmlRequest();
  }

  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction.
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForQuery(request->transaction(),
                                                      /*is_dml=*/true));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));

  if (txn->IsPartitionedDml()) {
    return error::BatchDmlOnlySupportsReadWriteTransaction();
  }

  // Set default response status to OK. Any error will override this.
  *response->mutable_status() = StatusToProto(absl::OkStatus());

  // Wrap all operations on this transaction so they are atomic.
  return txn->GuardedCall(Transaction::OpType::kDml, [&]() -> absl::Status {
    // Register DML request and check for status replay.
    const auto state = txn->LookupOrRegisterDmlRequest(
        request->seqno(), HashRequest(request), request->statements(0).sql());
    if (state.has_value()) {
      if (!state->status.ok() &&
          txn->DMLErrorType() ==
              Transaction::DMLErrorHandlingMode::kDmlRegistrationError) {
        return state->status;
      }
      if (!std::holds_alternative<spanner_api::ExecuteBatchDmlResponse>(
              state->outcome)) {
        return error::ReplayRequestMismatch(request->seqno(),
                                            request->statements(0).sql());
      }
      *response =
          std::get<spanner_api::ExecuteBatchDmlResponse>(state->outcome);

      // BatchDml always returns OK status with the error being populated in the
      // response.
      return absl::OkStatus();
    }
    // DML needs to explicitly check the transaction status since
    // the DML sequence number replay should take priority over returning
    // a previously encountered error status.
    GOOGLESQL_RETURN_IF_ERROR(txn->Status());

    // Cannot query after commit, rollback, or non-recoverable error.
    if (txn->IsInvalid()) {
      return error::CannotUseTransactionAfterConstraintError();
    }
    if (txn->IsCommitted() || txn->IsRolledback()) {
      return error::CannotReadOrQueryAfterCommitOrRollback();
    }

    for (int index = 0; index < request->statements_size(); ++index) {
      const auto& statement = request->statements(index);
      if (!backend::IsDMLQuery(statement.sql())) {
        absl::Status error = error::ExecuteBatchDmlOnlySupportsDmlStatements(
            index, statement.sql());
        *response->mutable_status() = StatusToProto(error);
        txn->SetDmlReplayOutcome(*response);
        return absl::OkStatus();
      }

      const auto maybe_result = ExecuteQuery(
          statement, txn,
          request->request_options().client_context().secure_context());
      if (!maybe_result.ok() &&
          maybe_result.status().code() != absl::StatusCode::kAborted) {
        absl::Status error = maybe_result.status();
        *response->mutable_status() = StatusToProto(error);
        txn->SetDmlReplayOutcome(*response);
        txn->MaybeInvalidate(error);
        if (!txn->IsInvalid() &&
            ShouldReturnTransaction(request->transaction()) && index == 0) {
          // The transaction ID has not been returned to the user yet, so we
          // must rollback the transaction to avoid leaving it in an active
          // state.
          txn->Rollback().IgnoreError();
        }
        return absl::OkStatus();
      } else if (maybe_result.status().code() == absl::StatusCode::kAborted) {
        return maybe_result.status();
      }

      const auto& result = maybe_result.value();
      spanner_api::ResultSet* result_set = response->add_result_sets();
      result_set->mutable_stats()->set_row_count_exact(
          result.modified_row_count);

      // Only populate metadata for first result set.
      if (index == 0) {
        result_set->mutable_metadata()->mutable_row_type();
        if (ShouldReturnTransaction(request->transaction())) {
          GOOGLESQL_ASSIGN_OR_RETURN(
              *result_set->mutable_metadata()->mutable_transaction(),
              txn->ToProto());
        }
      }
    }

    if (txn->IsReadWrite() && session->multiplexed()) {
      response->mutable_precommit_token();
    }

    // Set the replay outcome.
    txn->SetDmlReplayOutcome(*response);
    return absl::OkStatus();
  });
}
REGISTER_GRPC_HANDLER(Spanner, ExecuteBatchDml);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
