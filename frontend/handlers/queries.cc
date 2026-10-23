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

#include <algorithm>
#include <cstdint>
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
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"
#include "backend/access/read.h"
#include "backend/database/database.h"
#include "backend/query/change_stream/change_stream_query_validator.h"
#include "backend/query/query_engine.h"
#include "backend/query/queue/queryable_queue_tvfs.h"
#include "backend/storage/storage.h"
#include "common/config.h"
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

struct QueueReceiveStreamState {
  backend::QueueReceiveMetadata metadata;
  backend::Query query;
  absl::Time stream_start_time = absl::Now();
  int64_t total_output_rows = 0;
  bool sent_metadata = false;
  spanner_api::PartialResultSet empty_metadata_response;
};

absl::Status ExecuteQueueReceivePolling(
    RequestContext* ctx, const spanner_api::ExecuteSqlRequest* request,
    std::shared_ptr<Session> session, QueueReceiveStreamState* state,
    ServerStream<spanner_api::PartialResultSet>* stream) {
  const absl::Time deadline =
      state->stream_start_time + state->metadata.max_duration;
  spanner_api::TransactionOptions poll_txn_options;
  poll_txn_options.mutable_read_only()->set_strong(true);

  while (absl::Now() < deadline) {
    if (ctx->grpc() != nullptr && ctx->grpc()->IsCancelled()) {
      return absl::CancelledError();
    }
    absl::Duration remaining = deadline - absl::Now();
    if (remaining <= absl::ZeroDuration()) {
      break;
    }
    absl::SleepFor(std::min(absl::Milliseconds(50), remaining));
    if (ctx->grpc() != nullptr && ctx->grpc()->IsCancelled()) {
      return absl::CancelledError();
    }
    GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> poll_txn,
                     session->CreateSingleUseTransaction(poll_txn_options));
    GOOGLESQL_RETURN_IF_ERROR(
        poll_txn->GuardedCall(Transaction::OpType::kSql, [&]() -> absl::Status {
          GOOGLESQL_ASSIGN_OR_RETURN(
              backend::QueryResult poll_result,
              poll_txn->ExecuteSql(state->query, request->query_mode()));
          if (poll_result.num_output_rows == 0) {
            return absl::OkStatus();
          }
          state->total_output_rows += poll_result.num_output_rows;
          GOOGLESQL_ASSIGN_OR_RETURN(
              std::vector<spanner_api::PartialResultSet> poll_responses,
              RowCursorToPartialResultSetProtos(poll_result.rows.get(),
                                                /*limit=*/0));
          if (state->sent_metadata) {
            poll_responses.front().clear_metadata();
          } else {
            if (ShouldReturnTransaction(request->transaction())) {
              GOOGLESQL_ASSIGN_OR_RETURN(*poll_responses.front()
                                    .mutable_metadata()
                                    ->mutable_transaction(),
                               poll_txn->ToProto());
            }
            GOOGLESQL_RETURN_IF_ERROR(AddUndeclaredParametersFromQueryResult(
                &poll_result.parameter_types,
                poll_responses.front().mutable_metadata()));
            state->sent_metadata = true;
          }
          for (const auto& response : poll_responses) {
            stream->Send(response);
          }
          return absl::OkStatus();
        }));
  }
  if (!state->sent_metadata) {
    if (request->query_mode() == spanner_api::ExecuteSqlRequest::PROFILE) {
      backend::QueryResult total_result;
      total_result.num_output_rows = state->total_output_rows;
      total_result.elapsed_time = absl::Now() - state->stream_start_time;
      AddQueryStatsFromQueryResult(
          total_result, state->empty_metadata_response.mutable_stats()
                            ->mutable_query_stats());
    }
    stream->Send(state->empty_metadata_response);
  } else if (request->query_mode() == spanner_api::ExecuteSqlRequest::PROFILE) {
    spanner_api::PartialResultSet stats_response;
    backend::QueryResult total_result;
    total_result.num_output_rows = state->total_output_rows;
    total_result.elapsed_time = absl::Now() - state->stream_start_time;
    AddQueryStatsFromQueryResult(
        total_result, stats_response.mutable_stats()->mutable_query_stats());
    stream->Send(stats_response);
  }
  return absl::OkStatus();
}

// Emulator-only diagnostic statements, answered when
// --enable_emulator_diagnostic_statements is set:
//
//   SELECT EMULATOR_RECLAIM(['Table', ...] [, name => value, ...])
//   SELECT EMULATOR_LOCK_STATS()
//
// Each is recognised only as a whole statement, so it cannot match a real
// query. With the flag off they reach the query engine and fail there as
// unknown functions, as they do on Cloud Spanner.
constexpr absl::string_view kReclaimPrefix = "SELECT EMULATOR_RECLAIM(";
constexpr absl::string_view kLockStatsStatement = "SELECT EMULATOR_LOCK_STATS()";

// Returns the statement without surrounding whitespace or a trailing ';'.
absl::string_view TrimStatement(absl::string_view sql) {
  absl::string_view statement = absl::StripAsciiWhitespace(sql);
  absl::ConsumeSuffix(&statement, ";");
  return absl::StripAsciiWhitespace(statement);
}

bool IsEmulatorReclaimStatement(absl::string_view sql) {
  absl::string_view statement = TrimStatement(sql);
  return absl::StartsWithIgnoreCase(statement, kReclaimPrefix) &&
         absl::EndsWith(statement, ")");
}

bool IsEmulatorLockStatsStatement(absl::string_view sql) {
  return absl::EqualsIgnoreCase(TrimStatement(sql), kLockStatsStatement);
}

// Removes one pair of surrounding single quotes, if present.
absl::string_view Unquote(absl::string_view value) {
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

// Splits the text between the parentheses at commas outside single quotes.
std::vector<absl::string_view> SplitReclaimArguments(absl::string_view sql) {
  absl::string_view arguments = TrimStatement(sql);
  arguments.remove_prefix(kReclaimPrefix.size());
  arguments.remove_suffix(1);

  std::vector<absl::string_view> split;
  bool in_quotes = false;
  size_t start = 0;
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i] == '\'') {
      in_quotes = !in_quotes;
    } else if (arguments[i] == ',' && !in_quotes) {
      split.push_back(
          absl::StripAsciiWhitespace(arguments.substr(start, i - start)));
      start = i + 1;
    }
  }
  absl::string_view last = absl::StripAsciiWhitespace(arguments.substr(start));
  if (!last.empty() || !split.empty()) {
    split.push_back(last);
  }
  return split;
}

struct ReclaimArguments {
  std::vector<std::string> table_names;
  backend::PurgeWindow window;
  bool delete_rows = false;
};

absl::Status ReclaimArgumentError(absl::string_view message) {
  return absl::InvalidArgumentError(
      absl::StrCat("EMULATOR_RECLAIM: ", message));
}

absl::StatusOr<absl::Time> ParseReclaimTimestamp(absl::string_view name,
                                                 absl::string_view value) {
  absl::Time parsed;
  std::string parse_error;
  if (!absl::ParseTime(absl::RFC3339_full, value, &parsed, &parse_error)) {
    return ReclaimArgumentError(absl::StrCat("could not parse ", name, " \"",
                                             value, "\": ", parse_error));
  }
  return parsed;
}

// Parses the argument list. An argument that is not understood is rejected:
// a silently dropped bound would widen a sweep that can delete rows.
absl::StatusOr<ReclaimArguments> ParseReclaimArguments(absl::string_view sql,
                                                       absl::Time now) {
  ReclaimArguments parsed;
  bool has_not_after = false;
  for (absl::string_view argument : SplitReclaimArguments(sql)) {
    size_t arrow = argument.find("=>");
    if (arrow == absl::string_view::npos) {
      absl::string_view table_name = Unquote(argument);
      if (table_name.empty() || table_name.size() == argument.size()) {
        return ReclaimArgumentError(absl::StrCat(
            "expected a quoted table name or name => value, got \"", argument,
            "\""));
      }
      parsed.table_names.emplace_back(table_name);
      continue;
    }
    const std::string name = absl::AsciiStrToLower(
        absl::StripAsciiWhitespace(argument.substr(0, arrow)));
    const absl::string_view value =
        Unquote(absl::StripAsciiWhitespace(argument.substr(arrow + 2)));
    if (name == "not_before") {
      GOOGLESQL_ASSIGN_OR_RETURN(parsed.window.not_before,
                       ParseReclaimTimestamp(name, value));
    } else if (name == "not_after" || name == "not_after_lag") {
      if (has_not_after) {
        return ReclaimArgumentError(
            "pass at most one of not_after and not_after_lag");
      }
      has_not_after = true;
      if (name == "not_after") {
        GOOGLESQL_ASSIGN_OR_RETURN(parsed.window.not_after,
                         ParseReclaimTimestamp(name, value));
      } else {
        absl::Duration lag;
        if (!absl::ParseDuration(value, &lag) || lag < absl::ZeroDuration()) {
          return ReclaimArgumentError(absl::StrCat(
              "could not parse not_after_lag \"", value, "\""));
        }
        parsed.window.not_after = now - lag;
      }
    } else if (name == "delete_rows") {
      if (absl::EqualsIgnoreCase(value, "true")) {
        parsed.delete_rows = true;
      } else if (!absl::EqualsIgnoreCase(value, "false")) {
        return ReclaimArgumentError(absl::StrCat(
            "delete_rows must be true or false, got \"", value, "\""));
      }
    } else {
      return ReclaimArgumentError(
          absl::StrCat("unknown argument \"", name, "\""));
    }
  }
  if (parsed.window.not_before.has_value() &&
      parsed.window.not_after.has_value() &&
      *parsed.window.not_before >= *parsed.window.not_after) {
    return ReclaimArgumentError("not_before must be earlier than not_after");
  }
  // Unbounded, delete_rows would erase every live row in scope.
  if (parsed.delete_rows && !parsed.window.not_before.has_value() &&
      !parsed.window.not_after.has_value()) {
    return ReclaimArgumentError(
        "delete_rows requires not_before, not_after or not_after_lag");
  }
  return parsed;
}

void AddColumn(absl::string_view name, google::spanner::v1::TypeCode code,
               spanner_api::ResultSet* response) {
  auto* field = response->mutable_metadata()->mutable_row_type()->add_fields();
  field->set_name(std::string(name));
  field->mutable_type()->set_code(code);
}

absl::Status HandleEmulatorReclaim(const Session& session,
                                   absl::string_view sql,
                                   spanner_api::ResultSet* response) {
  GOOGLESQL_ASSIGN_OR_RETURN(ReclaimArguments arguments,
                   ParseReclaimArguments(sql, absl::Now()));
  const backend::Database::ReclaimStats stats =
      session.database()->backend()->ReclaimStorage(
          arguments.table_names, arguments.window, arguments.delete_rows);

  using google::spanner::v1::TypeCode;
  AddColumn("rows_purged", TypeCode::INT64, response);
  AddColumn("versions_purged", TypeCode::INT64, response);
  AddColumn("rows_deleted", TypeCode::INT64, response);
  auto* row = response->add_rows();
  // INT64 values travel as decimal strings on the wire.
  row->add_values()->set_string_value(absl::StrCat(stats.rows_purged));
  row->add_values()->set_string_value(absl::StrCat(stats.versions_purged));
  row->add_values()->set_string_value(absl::StrCat(stats.rows_deleted));
  return absl::OkStatus();
}

absl::Status HandleEmulatorLockStats(const Session& session,
                                     spanner_api::ResultSet* response) {
  using google::spanner::v1::TypeCode;
  AddColumn("table_name", TypeCode::STRING, response);
  AddColumn("whole_table_conflicts", TypeCode::INT64, response);
  AddColumn("bounded_conflicts", TypeCode::INT64, response);
  AddColumn("shared_conflicts", TypeCode::INT64, response);
  AddColumn("exclusive_conflicts", TypeCode::INT64, response);
  AddColumn("last_conflict_time", TypeCode::TIMESTAMP, response);
  for (const auto& [table_name, stats] :
       session.database()->backend()->LockConflictStatsByTable()) {
    auto* row = response->add_rows();
    row->add_values()->set_string_value(table_name);
    row->add_values()->set_string_value(
        absl::StrCat(stats.whole_table_conflicts));
    row->add_values()->set_string_value(absl::StrCat(stats.bounded_conflicts));
    row->add_values()->set_string_value(absl::StrCat(stats.shared_conflicts));
    row->add_values()->set_string_value(
        absl::StrCat(stats.exclusive_conflicts));
    row->add_values()->set_string_value(
        absl::FormatTime("%Y-%m-%dT%H:%M:%E*SZ", stats.last_conflict_time,
                         absl::UTCTimeZone()));
  }
  return absl::OkStatus();
}

// Answers an emulator diagnostic statement into `response`. Returns false when
// the flag is off or `sql` is not one, so the caller runs it as a query. These
// statements take no transaction, so this runs before transaction setup.
absl::StatusOr<bool> MaybeHandleEmulatorDiagnosticStatement(
    const Session& session, absl::string_view sql,
    spanner_api::ResultSet* response) {
  if (!config::enable_emulator_diagnostic_statements()) {
    return false;
  }
  if (IsEmulatorReclaimStatement(sql)) {
    GOOGLESQL_RETURN_IF_ERROR(HandleEmulatorReclaim(session, sql, response));
    return true;
  }
  if (IsEmulatorLockStatsStatement(sql)) {
    GOOGLESQL_RETURN_IF_ERROR(HandleEmulatorLockStats(session, response));
    return true;
  }
  return false;
}

// Returns the whole result set as a single streaming chunk.
spanner_api::PartialResultSet ToPartialResultSet(
    const spanner_api::ResultSet& result_set) {
  spanner_api::PartialResultSet partial;
  *partial.mutable_metadata() = result_set.metadata();
  for (const auto& row : result_set.rows()) {
    for (const auto& value : row.values()) {
      *partial.add_values() = value;
    }
  }
  return partial;
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

  GOOGLESQL_ASSIGN_OR_RETURN(bool handled,
                   MaybeHandleEmulatorDiagnosticStatement(
                       *session, request->sql(), response));
  if (handled) {
    return absl::OkStatus();
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

  spanner_api::ResultSet diagnostic_result;
  GOOGLESQL_ASSIGN_OR_RETURN(bool handled,
                   MaybeHandleEmulatorDiagnosticStatement(
                       *session, request->sql(), &diagnostic_result));
  if (handled) {
    stream->Send(ToPartialResultSet(diagnostic_result));
    return absl::OkStatus();
  }

  backend::ChangeStreamQueryValidator::ChangeStreamMetadata
      change_stream_metadata;
  QueueReceiveStreamState queue_receive_state;

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
            backend::Query query,
            QueryFromProto(
                request->sql(), request->params(), request->param_types(),
                txn->query_engine()->type_factory(),
                txn->schema()->proto_bundle(),
                request->request_options().client_context().secure_context()));
        query.queue_receive_metadata = &queue_receive_state.metadata;
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

        if (queue_receive_state.metadata.is_queue_receive_query) {
          queue_receive_state.query = query;
          queue_receive_state.total_output_rows = result.num_output_rows;
          if (result.num_output_rows > 0) {
            for (const auto& response : responses) {
              stream->Send(response);
            }
            queue_receive_state.sent_metadata = true;
          } else {
            queue_receive_state.empty_metadata_response = responses.front();
          }
          return absl::OkStatus();
        }

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
  if (status.ok() && queue_receive_state.metadata.is_queue_receive_query) {
    return ExecuteQueueReceivePolling(ctx, request, session,
                                      &queue_receive_state, stream);
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
