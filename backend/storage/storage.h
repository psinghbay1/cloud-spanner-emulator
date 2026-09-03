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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_STORAGE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_STORAGE_H_

#include <cstdint>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Storage defines the interface for a multi-version data store.
//
// There will be a Storage instance for each database created. The current
// interface is grow-only, i.e. once data is added, it will not be deleted.
// Storage is thread-safe.
class Storage {
 public:
  virtual ~Storage() {}

  // Returns the column values for the given key at the specified timestamp.
  // Returns NOT_FOUND if the given key does not exist. For a given row if the
  // column value is not set, it returns an invalid googlesql::Value, i.e. one
  // for which is_valid() is false. Column values are always set in order of the
  // columns defined in column_ids.
  virtual absl::Status Lookup(absl::Time timestamp, const TableID& table_id,
                              const Key& key,
                              const std::vector<ColumnID>& column_ids,
                              std::vector<googlesql::Value>* values) const = 0;

  // Returns zero or more rows for given key range. Keys are returned in
  // sorted order. See comments on StorageIterator for more details. KeyRange
  // interval should be in KeyRange::ClosedOpen format. Non ClosedOpen ranges
  // will result in INVALID_ARGUMENT.
  virtual absl::Status Read(absl::Time timestamp, const TableID& table_id,
                            const KeyRange& key_range,
                            const std::vector<ColumnID>& column_ids,
                            std::unique_ptr<StorageIterator>* itr) const = 0;

  // Writes column values for given key at the specified timestamp. Column value
  // will be overwritten for non-unique <timestamp, table_id, key, column_id>
  // combination.
  virtual absl::Status Write(absl::Time timestamp, const TableID& table_id,
                             const Key& key,
                             const std::vector<ColumnID>& column_ids,
                             const std::vector<googlesql::Value>& values) = 0;

  // Marks the given key range as deleted at the specified timestamp. Column
  // values at older timestamps are still accessible via Read and Lookup.
  // KeyRange interval should be in KeyRange::ClosedOpen format. Non ClosedOpen
  // ranges will result in INVALID_ARGUMENT.
  virtual absl::Status Delete(absl::Time timestamp, const TableID& table_id,
                              const KeyRange& key_range) = 0;

  // Sets the version retention period from the database options.
  // This is used to determine when to delete expired data from storage.
  virtual void SetVersionRetentionPeriod(
      absl::Duration version_retention_period) = 0;

  virtual void CleanUpDeletedTables(absl::Time timestamp) = 0;
  virtual void CleanUpDeletedColumns(absl::Time timestamp) = 0;

  virtual void MarkDroppedTable(absl::Time timestamp,
                                TableID dropped_table_id) = 0;

  virtual void MarkDroppedColumn(absl::Time timestamp, TableID dropped_table_id,
                                 ColumnID dropped_column_id) = 0;

  // Erases rows whose latest version is a deletion marker older than the
  // version retention period, including the row key itself.
  //
  // Delete() only appends a tombstone; the row key is never removed, and
  // RemoveExpiredVersions() can only trim a cell that is being written again.
  // A row that is deleted and never touched again therefore occupies memory
  // for the lifetime of the process. This reclaims those rows.
  //
  // A row is only erased when it is invisible at every timestamp a transaction
  // may legally read, i.e. its deletion marker is older than
  // `timestamp - version_retention_period`, using the same arithmetic as
  // CleanUpDeletedTables().
  //
  // `table_ids` scopes the sweep; an empty vector sweeps every table. Returns
  // the number of row keys erased.
  virtual int64_t PurgeExpiredDeletedRows(
      absl::Time timestamp, const std::vector<TableID>& table_ids) = 0;

  // Erases superseded versions of LIVE rows that are older than the version
  // retention period.
  //
  // RemoveExpiredVersions() only runs while its own cell is being written, so a
  // row that is updated a few times and then left alone keeps every superseded
  // version for the lifetime of the process. This sweeps them without needing a
  // write to each cell.
  //
  // The newest version at or before the retention floor is kept, so reads
  // anywhere inside the retention window still resolve correctly.
  //
  // `table_ids` scopes the sweep; an empty vector sweeps every table. Returns
  // the number of versions erased.
  virtual int64_t PurgeExpiredVersions(
      absl::Time timestamp, const std::vector<TableID>& table_ids) = 0;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_STORAGE_H_
