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

#include "backend/query/queryable_table.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"  //
#include "absl/types/span.h"
#include "backend/access/read.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/queryable_column.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/table.h"
#include "common/config.h"
#include "common/constants.h"
#include "common/feature_flags.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// An implementation of EvaluatorTableIterator which wraps a RowCursor.
//
// Used by QueryableTable::CreateEvaluatorTableIterator.
class RowCursorEvaluatorTableIterator
    : public googlesql::EvaluatorTableIterator {
 public:
  explicit RowCursorEvaluatorTableIterator(std::unique_ptr<RowCursor> cursor)
      : cursor_(std::move(cursor)) {
    values_.reserve(cursor_->NumColumns());
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      values_.push_back(googlesql::values::Null(cursor_->ColumnType(i)));
    }
  }

  int NumColumns() const override { return cursor_->NumColumns(); }

  std::string GetColumnName(int i) const override {
    return cursor_->ColumnName(i);
  }

  const googlesql::Type* GetColumnType(int i) const override {
    return cursor_->ColumnType(i);
  }

  bool NextRow() override {
    if (cursor_->Next()) {
      for (int i = 0; i < cursor_->NumColumns(); ++i) {
        values_[i] = cursor_->ColumnValue(i);
      }
      return true;
    } else {
      return false;
    }
  }

  const googlesql::Value& GetValue(int i) const override { return values_[i]; }

  absl::Status Status() const override { return cursor_->Status(); }

  // Cancel is best-effort and not required.
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  // The wrapped RowCursor.
  std::unique_ptr<RowCursor> cursor_;

  // Values of the current row. EvaluatorTableIterator::GetValue need to return
  // a reference so we need to buffer the values instead of simply delegate to
  // RowCursor::ColumnValue.
  std::vector<googlesql::Value> values_;
};

namespace {

using ColumnFilterMap =
    absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>;

// Above this many key combinations the scan reads a shorter key prefix.
constexpr int kMaxPushdownKeyRanges = 1000;

// Types whose SQL equality is exact value equality, so a key built from a
// filter value matches exactly the rows the filter matches.
bool IsPushdownKeyType(const googlesql::Type* type) {
  return type->IsInt64() || type->IsString() || type->IsBytes() ||
         type->IsBool() || type->IsDate() || type->IsTimestamp();
}

// Returns the values `filter` pins a column of `column_type` to, or nullopt
// when it is not an equality or IN list over values of exactly that type.
std::optional<std::vector<googlesql::Value>> EqualityValues(
    const googlesql::ColumnFilter& filter,
    const googlesql::Type* column_type) {
  std::vector<googlesql::Value> values;
  switch (filter.kind()) {
    case googlesql::ColumnFilter::kRange:
      if (!filter.lower_bound().is_valid() ||
          !filter.upper_bound().is_valid() ||
          !filter.lower_bound().Equals(filter.upper_bound())) {
        return std::nullopt;
      }
      values.push_back(filter.lower_bound());
      break;
    case googlesql::ColumnFilter::kInList:
      values = filter.in_list();
      break;
    default:
      return std::nullopt;
  }
  if (values.empty()) {
    return std::nullopt;
  }
  for (const googlesql::Value& value : values) {
    if (!value.is_valid() || value.is_null() ||
        !value.type()->Equals(column_type)) {
      return std::nullopt;
    }
  }
  return values;
}

// Returns a usable bound of a range filter on a column of `column_type`:
// nullopt for an invalid (unbounded) bound, or `rejected` set when the bound
// cannot be used at all.
std::optional<googlesql::Value> RangeBound(const googlesql::Value& bound,
                                           const googlesql::Type* column_type,
                                           bool* rejected) {
  if (!bound.is_valid()) {
    return std::nullopt;
  }
  if (bound.is_null() || !bound.type()->Equals(column_type)) {
    *rejected = true;
    return std::nullopt;
  }
  return bound;
}

// Narrows each key prefix to one closed range over the next key column when
// that column has a range filter. A closed endpoint that is a key prefix
// covers every key with that prefix, so an unbounded side is just the prefix.
// Later key columns are ignored, which over-locks but never misses a row.
std::optional<KeySet> RangeKeySetAfterPrefix(
    const Table* table, int table_column_index, size_t prefix_length,
    const absl::flat_hash_map<int, const googlesql::ColumnFilter*>&
        filter_by_column,
    const std::vector<std::vector<googlesql::Value>>& key_prefixes) {
  auto filter = filter_by_column.find(table_column_index);
  if (filter == filter_by_column.end() ||
      filter->second->kind() != googlesql::ColumnFilter::kRange) {
    return std::nullopt;
  }
  const googlesql::Type* column_type =
      table->columns()[table_column_index]->GetType();
  if (!IsPushdownKeyType(column_type)) {
    return std::nullopt;
  }
  bool rejected = false;
  std::optional<googlesql::Value> lower =
      RangeBound(filter->second->lower_bound(), column_type, &rejected);
  std::optional<googlesql::Value> upper =
      RangeBound(filter->second->upper_bound(), column_type, &rejected);
  if (rejected || (!lower.has_value() && !upper.has_value()) ||
      (lower.has_value() && upper.has_value() && upper->LessThan(*lower))) {
    return std::nullopt;
  }
  // Storage order runs from the upper bound down on a descending column.
  if (table->primary_key()[prefix_length]->is_descending()) {
    std::swap(lower, upper);
  }
  KeySet key_set;
  for (const std::vector<googlesql::Value>& prefix : key_prefixes) {
    std::vector<googlesql::Value> start = prefix;
    std::vector<googlesql::Value> limit = prefix;
    if (lower.has_value()) {
      start.push_back(*lower);
    }
    if (upper.has_value()) {
      limit.push_back(*upper);
    }
    key_set.AddRange(KeyRange::ClosedClosed(Key(std::move(start)),
                                            Key(std::move(limit))));
  }
  return key_set;
}

using FilterByColumn = absl::flat_hash_map<int, const googlesql::ColumnFilter*>;

// Re-keys the evaluator's filters from scan position to table column index.
FilterByColumn FiltersByTableColumn(absl::Span<const int> scan_column_idxs,
                                    const ColumnFilterMap& filter_map) {
  FilterByColumn filter_by_column;
  for (const auto& [scan_index, filter] : filter_map) {
    if (filter != nullptr && scan_index >= 0 &&
        scan_index < static_cast<int>(scan_column_idxs.size())) {
      filter_by_column[scan_column_idxs[scan_index]] = filter.get();
    }
  }
  return filter_by_column;
}

// Builds the KeySet a scan must read from the equality filters on the leading
// key columns, then a range filter on the next one. Anything it cannot use
// widens the set, never narrows it: the evaluator re-applies the full WHERE to
// every row read.
KeySet KeySetForKeyFilters(const Table* table,
                           absl::Span<const int> primary_key_column_indexes,
                           const FilterByColumn& filter_by_column) {
  std::vector<std::vector<googlesql::Value>> key_prefixes = {{}};
  for (int table_column_index : primary_key_column_indexes) {
    auto filter = filter_by_column.find(table_column_index);
    if (filter == filter_by_column.end()) {
      break;
    }
    const googlesql::Type* column_type =
        table->columns()[table_column_index]->GetType();
    if (!IsPushdownKeyType(column_type)) {
      break;
    }
    std::optional<std::vector<googlesql::Value>> values =
        EqualityValues(*filter->second, column_type);
    if (!values.has_value() ||
        key_prefixes.size() * values->size() >
            static_cast<size_t>(kMaxPushdownKeyRanges)) {
      break;
    }
    std::vector<std::vector<googlesql::Value>> extended_prefixes;
    extended_prefixes.reserve(key_prefixes.size() * values->size());
    for (const std::vector<googlesql::Value>& prefix : key_prefixes) {
      for (const googlesql::Value& value : *values) {
        std::vector<googlesql::Value> extended = prefix;
        extended.push_back(value);
        extended_prefixes.push_back(std::move(extended));
      }
    }
    key_prefixes = std::move(extended_prefixes);
  }

  const size_t prefix_length = key_prefixes.front().size();
  if (prefix_length < primary_key_column_indexes.size()) {
    std::optional<KeySet> ranged = RangeKeySetAfterPrefix(
        table, primary_key_column_indexes[prefix_length], prefix_length,
        filter_by_column, key_prefixes);
    if (ranged.has_value()) {
      return *std::move(ranged);
    }
  }
  if (prefix_length == 0) {
    return KeySet::All();
  }
  const bool is_full_key = prefix_length == primary_key_column_indexes.size();
  KeySet key_set;
  for (std::vector<googlesql::Value>& prefix : key_prefixes) {
    Key key(std::move(prefix));
    if (is_full_key) {
      key_set.AddKey(key);
    } else {
      key_set.AddRange(KeyRange::Prefix(key));
    }
  }
  return key_set;
}

bool IsAllKeys(const KeySet& key_set) {
  return key_set.keys().empty() && key_set.ranges().size() == 1 &&
         key_set.ranges().front() == KeyRange::All();
}

int ColumnIndexIn(const Table* table, const Column* column) {
  const auto& columns = table->columns();
  auto it = std::find(columns.begin(), columns.end(), column);
  return it == columns.end() ? -1 : static_cast<int>(it - columns.begin());
}

// Above this many index entries the scan reads the whole base table.
constexpr int kMaxIndexLookupKeys = 1000;

// Reads, in the scan's transaction, the range of `index` the filters allow
// and returns the base-table keys of every entry in it. The shared lock on
// that index range is the phantom protection: any write that could change the
// answer writes an entry in the range. Returns nullopt when the filters do not
// bind a leading index key column, or the range holds too many entries.
absl::StatusOr<std::optional<KeySet>> BaseKeysFromIndexRange(
    RowReader* reader, const Table* base, const Index* index,
    const FilterByColumn& base_filters) {
  const Table* data_table = index->index_data_table();
  FilterByColumn index_filters;
  std::vector<int> index_key_column_indexes;
  for (const KeyColumn* key_column : data_table->primary_key()) {
    const int data_index = ColumnIndexIn(data_table, key_column->column());
    if (data_index < 0) {
      return std::nullopt;
    }
    index_key_column_indexes.push_back(data_index);
    const int base_index =
        ColumnIndexIn(base, key_column->column()->source_column());
    auto filter = base_filters.find(base_index);
    if (filter != base_filters.end()) {
      index_filters[data_index] = filter->second;
    }
  }
  KeySet index_key_set =
      KeySetForKeyFilters(data_table, index_key_column_indexes, index_filters);
  if (IsAllKeys(index_key_set)) {
    return std::nullopt;
  }

  // The base primary key, as index data table columns, in base key order.
  ReadArg index_read;
  index_read.table = base->Name();
  index_read.index = index->Name();
  index_read.key_set = std::move(index_key_set);
  index_read.allow_pending_commit_timestamps = true;
  for (const KeyColumn* base_key_column : base->primary_key()) {
    const Column* data_column = nullptr;
    for (const Column* column : data_table->columns()) {
      if (column->source_column() == base_key_column->column()) {
        data_column = column;
        break;
      }
    }
    if (data_column == nullptr) {
      return std::nullopt;
    }
    index_read.columns.push_back(data_column->Name());
  }

  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader->Read(index_read, &cursor));
  KeySet base_keys;
  int entries = 0;
  while (cursor->Next()) {
    if (++entries > kMaxIndexLookupKeys) {
      return std::nullopt;
    }
    std::vector<googlesql::Value> key_values;
    key_values.reserve(cursor->NumColumns());
    for (int i = 0; i < cursor->NumColumns(); ++i) {
      key_values.push_back(cursor->ColumnValue(i));
    }
    base_keys.AddKey(Key(std::move(key_values)));
  }
  GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
  return base_keys;
}

// An EvaluatorTableIterator that reads only once the evaluator has passed its
// column filters (or asks for the first row), so the read, and the locks it
// takes, cover just the keys those filters allow.
class DeferredReadEvaluatorTableIterator
    : public googlesql::EvaluatorTableIterator {
 public:
  DeferredReadEvaluatorTableIterator(
      RowReader* reader, ReadArg read_arg, const Table* table,
      std::vector<int> primary_key_column_indexes,
      std::vector<int> scan_column_idxs,
      std::vector<const googlesql::Type*> column_types,
      absl::flat_hash_map<int, googlesql::ColumnFilter> dml_filters,
      std::vector<const Index*> forced_indexes)
      : reader_(reader),
        read_arg_(std::move(read_arg)),
        table_(table),
        primary_key_column_indexes_(std::move(primary_key_column_indexes)),
        scan_column_idxs_(std::move(scan_column_idxs)),
        column_types_(std::move(column_types)),
        dml_filters_(std::move(dml_filters)),
        forced_indexes_(std::move(forced_indexes)) {
    values_.reserve(column_types_.size());
    for (const googlesql::Type* type : column_types_) {
      values_.push_back(googlesql::values::Null(type));
    }
  }

  int NumColumns() const override { return column_types_.size(); }

  std::string GetColumnName(int i) const override {
    return read_arg_.columns[i];
  }

  const googlesql::Type* GetColumnType(int i) const override {
    return column_types_[i];
  }

  absl::Status SetColumnFilterMap(ColumnFilterMap filter_map) override {
    if (cursor_ != nullptr) {
      return absl::OkStatus();
    }
    FilterByColumn filter_by_column =
        FiltersByTableColumn(scan_column_idxs_, filter_map);
    // The evaluator may also filter a DML scan; keep the WHERE's keys too.
    for (const auto& [table_column_index, filter] : dml_filters_) {
      filter_by_column.try_emplace(table_column_index, &filter);
    }
    read_arg_.key_set = KeySetForKeyFilters(table_, primary_key_column_indexes_,
                                            filter_by_column);
    // No base key prefix: try a FORCE_INDEX index. Every base row of every
    // entry in the index range is read, so all of them get point locks.
    for (const Index* index : forced_indexes_) {
      if (!IsAllKeys(read_arg_.key_set)) {
        break;
      }
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::optional<KeySet> base_keys,
          BaseKeysFromIndexRange(reader_, table_, index, filter_by_column));
      if (base_keys.has_value()) {
        read_arg_.key_set = *std::move(base_keys);
      }
    }
    return StartRead();
  }

  bool NextRow() override {
    if (cursor_ == nullptr) {
      read_status_ = StartRead();
      if (!read_status_.ok()) {
        return false;
      }
    }
    if (!cursor_->Next()) {
      return false;
    }
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      values_[i] = cursor_->ColumnValue(i);
    }
    return true;
  }

  const googlesql::Value& GetValue(int i) const override { return values_[i]; }

  absl::Status Status() const override {
    if (!read_status_.ok() || cursor_ == nullptr) {
      return read_status_;
    }
    return cursor_->Status();
  }

  // Cancel is best-effort and not required.
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  absl::Status StartRead() { return reader_->Read(read_arg_, &cursor_); }

  RowReader* reader_;
  ReadArg read_arg_;
  const Table* table_;
  std::vector<int> primary_key_column_indexes_;
  std::vector<int> scan_column_idxs_;
  std::vector<const googlesql::Type*> column_types_;
  absl::flat_hash_map<int, googlesql::ColumnFilter> dml_filters_;
  std::vector<const Index*> forced_indexes_;
  std::unique_ptr<RowCursor> cursor_;
  absl::Status read_status_;
  std::vector<googlesql::Value> values_;
};

}  // namespace

absl::StatusOr<std::unique_ptr<const googlesql::AnalyzerOutput>>
QueryableTable::AnalyzeColumnExpression(
    const Column* column, googlesql::TypeFactory* type_factory,
    googlesql::Catalog* catalog,
    std::optional<const googlesql::AnalyzerOptions> opt_options) const {
  std::unique_ptr<const googlesql::AnalyzerOutput> output = nullptr;
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  bool is_generated_column = enable_generated_pk && column->is_generated();
  if (opt_options.has_value() &&
      (column->has_default_value() || (is_generated_column))) {
    googlesql::AnalyzerOptions options = opt_options.value();
    if (is_generated_column) {
      for (const Column* dep : column->dependent_columns()) {
        GOOGLESQL_RETURN_IF_ERROR(
            options.AddExpressionColumn(dep->Name(), dep->GetType()))
            << "Failed to add dependent column " << dep->Name()
            << " for generated column : " << column->FullName();
      }
    }
    std::string expression_type = "default";
    if (is_generated_column) {
      expression_type = "generated";
    }
    GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeExpressionForAssignmentToType(
        column->expression().value(), options, catalog, type_factory,
        column->GetType(), &output))
        << "Failed to analyze " << expression_type << " expression for column "
        << column->FullName();
  }
  return std::move(output);
}

QueryableTable::QueryableTable(
    const backend::Table* table, RowReader* reader,
    std::optional<const googlesql::AnalyzerOptions> opt_options,
    googlesql::Catalog* catalog, googlesql::TypeFactory* type_factory,
    bool is_synonym, bool is_pg)
    : is_synonym_(is_synonym), wrapped_table_(table), reader_(reader) {
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  for (const auto* column : table->columns()) {
    absl::StatusOr<std::unique_ptr<const googlesql::AnalyzerOutput>>
        analyzer_output =
            AnalyzeColumnExpression(column, type_factory, catalog, opt_options);
    ABSL_CHECK_OK(analyzer_output.status());  // Crash OK
    std::unique_ptr<const googlesql::AnalyzerOutput> output =
        std::move(analyzer_output.value());
    bool is_generated_column = enable_generated_pk && column->is_generated();
    if (is_pg && table->owner_queue() != nullptr &&
        column->Name() == kQueueDeliverTime) {
      // In PostgreSQL dialect, expose snake_case `deliver_time` in `SELECT *`
      // and also register PascalCase `"DeliverTime"` as a pseudo-column.
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, /*output=*/nullptr, std::nullopt,
          std::string(kQueuePgDeliverTime), /*is_pseudo_column=*/false));
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, /*output=*/nullptr, std::nullopt,
          std::string(kQueueDeliverTime), /*is_pseudo_column=*/true));
      continue;
    }
    if (column->has_default_value() || (is_generated_column)) {
      googlesql::Column::ExpressionAttributes::ExpressionKind expression_kind =
          googlesql::Column::ExpressionAttributes::ExpressionKind::DEFAULT;
      if (is_generated_column) {
        expression_kind =
            googlesql::Column::ExpressionAttributes::ExpressionKind::GENERATED;
      }
      googlesql::Column::ExpressionAttributes expression_attributes =
          googlesql::Column::ExpressionAttributes(expression_kind,
                                                  column->expression().value(),
                                                  output->resolved_expr());
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output),
          std::make_optional(expression_attributes)));
    } else {
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output), std::nullopt));
    }
  }

  // Populate primary_key_column_indexes_.
  for (const auto& key_column : table->primary_key()) {
    for (int i = 0; i < wrapped_table_->columns().size(); ++i) {
      if (key_column->column() == wrapped_table_->columns()[i]) {
        primary_key_column_indexes_.push_back(i);
        break;
      }
    }
  }
}

absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
QueryableTable::CreateEvaluatorTableIterator(
    absl::Span<const int> column_idxs) const {
  GOOGLESQL_RET_CHECK_NE(reader_, nullptr);

  std::vector<std::string> column_names;
  for (int idx : column_idxs) {
    column_names.push_back(columns_[idx]->wrapped_column()->Name());
  }

  ReadArg read_arg;
  read_arg.table = FullName();
  read_arg.key_set = KeySet::All();
  read_arg.columns = column_names;
  // Pending commit timestamp restrictions for queries are implemented in
  // QueryValidator so we do not need enforcement during the read here.
  // Furthermore, without enabling this certain internal reads issued by the
  // GoogleSQL reference implementation will be rejected.
  read_arg.allow_pending_commit_timestamps = true;

  // If current table is a change stream internal data/partition table, change
  // the read arg to access internal tables directly.
  if (wrapped_table_->owner_change_stream() != nullptr) {
    absl::string_view change_stream_name = read_arg.table;
    if (absl::StartsWith(read_arg.table, kChangeStreamPartitionTablePrefix)) {
      absl::ConsumePrefix(&change_stream_name,
                          kChangeStreamPartitionTablePrefix);
      read_arg.change_stream_for_partition_table = change_stream_name;
    } else {
      absl::ConsumePrefix(&change_stream_name, kChangeStreamDataTablePrefix);
      read_arg.change_stream_for_data_table = change_stream_name;
    }
  }
  // Change stream and index tables keep the whole-table read.
  if (config::enable_sql_read_pushdown() && wrapped_table_->is_public()) {
    std::vector<const googlesql::Type*> column_types;
    column_types.reserve(column_idxs.size());
    for (int idx : column_idxs) {
      column_types.push_back(GetColumn(idx)->GetType());
    }
    if (!dml_key_filters_.empty()) {
      FilterByColumn dml_filter_by_column;
      for (const auto& [table_column_index, filter] : dml_key_filters_) {
        dml_filter_by_column[table_column_index] = &filter;
      }
      read_arg.key_set = KeySetForKeyFilters(
          wrapped_table_, primary_key_column_indexes_, dml_filter_by_column);
    }
    return std::make_unique<DeferredReadEvaluatorTableIterator>(
        reader_, std::move(read_arg), wrapped_table_,
        primary_key_column_indexes_,
        std::vector<int>(column_idxs.begin(), column_idxs.end()),
        std::move(column_types), dml_key_filters_, forced_indexes_);
  }
  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader_->Read(read_arg, &cursor));
  return std::make_unique<RowCursorEvaluatorTableIterator>(std::move(cursor));
}

const googlesql::Column* QueryableTable::FindColumnByName(
    const std::string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  for (const auto& column : columns_) {
    if (absl::EqualsIgnoreCase(column->Name(), name)) {
      return column.get();
    }
  }
  return nullptr;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
