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

#include "backend/schema/catalog/versioned_catalog.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/utils.h"
#include "backend/schema/catalog/schema.h"
#include "googlesql/base/ret_check.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

absl::StatusOr<absl::Duration> ParseVersionRetentionPeriod(
    absl::string_view version_retention_period) {
  int64_t retention_period = ParseSchemaTimeSpec(version_retention_period);

  if (retention_period <= 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid retention period: ", version_retention_period));
  } else if (retention_period > (7 * 24 * 60 * 60)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Retention period must be less than 7 days: ",
                     version_retention_period));
  } else {
    if (retention_period < 60 * 60) {
      ABSL_LOG(WARNING) << "Retention period has been set to less than 1 hour. "
                   << "This is only supported in the emulator. "
                   << "This is not supported by Spanner.";
    }
    return absl::Seconds(retention_period);
  }
}

}  // namespace

VersionedCatalog::VersionedCatalog() {
  schemas_[absl::InfinitePast()] = std::make_unique<const Schema>();
}

VersionedCatalog::VersionedCatalog(
    std::unique_ptr<const Schema> initial_schema) {
  schemas_[absl::InfinitePast()] = std::move(initial_schema);
}

const Schema* VersionedCatalog::GetSchema(absl::Time timestamp) const {
  absl::MutexLock lock(mu_);
  auto itr = schemas_.upper_bound(timestamp);
  itr--;
  return itr->second.get();
}

std::shared_ptr<const Schema> VersionedCatalog::GetSchemaShared(
    absl::Time timestamp) const {
  absl::MutexLock lock(mu_);
  auto itr = schemas_.upper_bound(timestamp);
  itr--;
  return itr->second;
}

const Schema* VersionedCatalog::GetLatestSchema() const {
  return GetSchema(absl::InfiniteFuture());
}

absl::Status VersionedCatalog::AddSchema(absl::Time creation_time,
                                         std::unique_ptr<const Schema> schema) {
  absl::MutexLock lock(mu_);
  GOOGLESQL_RET_CHECK(creation_time > schemas_.rbegin()->first)
      << "Failed to insert schema at " << absl::FormatTime(creation_time)
      << ": the latest schema creation timestamp is "
      << absl::FormatTime(schemas_.rbegin()->first);
  auto version_retention_period =
      ParseVersionRetentionPeriod(schema->version_retention_period());
  if (!version_retention_period.ok()) {
    return version_retention_period.status();
  }
  version_retention_period_ = *version_retention_period;
  schemas_[creation_time] = std::move(schema);
  // Pruned here because a schema load applies many DDL batches and nothing
  // after the last one runs RemoveExpiredSchemas, so every intermediate
  // version would otherwise live for the whole retention period.
  if (schema_version_retention_.has_value()) {
    PruneSchemasOlderThanLocked(creation_time - *schema_version_retention_);
  }
  return absl::OkStatus();
}

void VersionedCatalog::RemoveExpiredSchemas(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  PruneSchemasOlderThanLocked(timestamp - version_retention_period_);
}

int64_t VersionedCatalog::PruneSchemasOlderThan(absl::Time cutoff) {
  absl::MutexLock lock(mu_);
  return PruneSchemasOlderThanLocked(cutoff);
}

int64_t VersionedCatalog::PruneSchemasOlderThanLocked(absl::Time cutoff) {
  auto upper_bound = schemas_.upper_bound(cutoff);
  auto it = ++schemas_.begin();  // Skip the infinite past schema.
  int64_t erased = 0;
  while (it != upper_bound) {
    auto next = it;
    if (++next == upper_bound) {
      // The current schema needs to be kept: it is the one in effect at
      // `cutoff`, so reads at and after `cutoff` still resolve to it.
      break;
    }
    it = schemas_.erase(it);
    ++erased;
  }
  if (erased > 0) {
    // `it` is the kept covering version (or the latest). Anything older is
    // gone, so reads below its creation time must be refused, not served
    // with a newer schema.
    min_readable_time_ = std::max(min_readable_time_, it->first);
  }
  return erased;
}

absl::Time VersionedCatalog::LatestSchemaTime() const {
  absl::MutexLock lock(mu_);
  return schemas_.rbegin()->first;
}

absl::Time VersionedCatalog::MinReadableSchemaTime() const {
  absl::MutexLock lock(mu_);
  return min_readable_time_;
}

void VersionedCatalog::set_schema_version_retention(absl::Duration retention) {
  absl::MutexLock lock(mu_);
  schema_version_retention_ = retention;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
