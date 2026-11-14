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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_QUERYABLE_TABLE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_QUERYABLE_TABLE_H_

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "backend/access/read.h"
#include "backend/query/queryable_column.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// A wrapper over Table class which implements googlesql::Table.
// QueryableTable builds instances of EvalutorTableIterator by reading data of
// the table through a RowReader.
class QueryableTable : public googlesql::Table {
 public:
  // 'options' , 'catalog' , 'type_factory' must be non-null when specifying a
  // QueryableTable with default value columns.
  QueryableTable(
      const backend::Table* table, RowReader* reader,
      std::optional<const googlesql::AnalyzerOptions> options = std::nullopt,
      googlesql::Catalog* catalog = nullptr,
      googlesql::TypeFactory* type_factory = nullptr, bool is_synonym = false,
      bool is_pg = false);

  std::string Name() const override {
    return std::string(SDLObjectName::GetInSchemaName(SynonymOrName()));
  }

  // FullName includes schema if present.
  std::string FullName() const override { return SynonymOrName(); }

  int NumColumns() const override { return columns_.size(); }

  const googlesql::Column* GetColumn(int i) const override {
    return columns_[i].get();
  }

  const googlesql::Column* FindColumnByName(
      const std::string& name) const override;

  std::optional<std::vector<int>> PrimaryKey() const override {
    return primary_key_column_indexes_.empty()
               ? std::nullopt
               : std::make_optional(primary_key_column_indexes_);
  }

  const backend::Table* wrapped_table() const { return wrapped_table_; }

  // Records an index a FORCE_INDEX hint names on a scan of this table. Scans
  // may use it to lock an index range instead of the whole table. The catalog
  // is built per query, so this lives only for one query.
  void RecordForcedIndex(const Index* index) const {
    if (std::find(forced_indexes_.begin(), forced_indexes_.end(), index) ==
        forced_indexes_.end()) {
      forced_indexes_.push_back(index);
    }
  }

  // Override CreateEvaluatorTableIterator.
  absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
  CreateEvaluatorTableIterator(
      absl::Span<const int> column_idxs) const override;

  // googlesql pushes no column filters into a DML statement's table scan, so
  // the statement sets the key filters of its WHERE here, keyed by table
  // column index, for the duration of its execution.
  void SetDmlKeyFilters(
      absl::flat_hash_map<int, googlesql::ColumnFilter> filters) const {
    dml_key_filters_ = std::move(filters);
  }
  void ClearDmlKeyFilters() const { dml_key_filters_.clear(); }

 private:
  absl::StatusOr<std::unique_ptr<const googlesql::AnalyzerOutput>>
  AnalyzeColumnExpression(
      const Column* column, googlesql::TypeFactory* type_factory,
      googlesql::Catalog* catalog,
      std::optional<const googlesql::AnalyzerOptions> opt_options) const;

  // Whether the table should be treated as a synonym.
  bool is_synonym_;

  // Returns the name of the table or the synonym if the table is a synonym.
  std::string SynonymOrName() const {
    return is_synonym_ ? wrapped_table_->synonym() : wrapped_table_->Name();
  }

  // The underlying Table object which backes the QueryableTable.
  const backend::Table* wrapped_table_;

  // A RowReader which data of the table can be read from to build a
  // EvalutorTableIterator when CreateEvaluatorTableIterator is called.
  RowReader* reader_;

  // The columns in the table.
  std::vector<std::unique_ptr<const QueryableColumn>> columns_;

  // A list of ordinal indexes of the primary key columns of the table.
  std::vector<int> primary_key_column_indexes_;

  mutable absl::flat_hash_map<int, googlesql::ColumnFilter> dml_key_filters_;
  // Indexes named by FORCE_INDEX hints on scans of this table in this query.
  mutable std::vector<const Index*> forced_indexes_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_QUERYABLE_TABLE_H_
