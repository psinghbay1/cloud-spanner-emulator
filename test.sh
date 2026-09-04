#!/usr/bin/env bash
#
# Run every test covering the memory-reclamation changes on this branch.
#
#   ./test.sh              # run them all
#   ./test.sh --list       # list the test cases, run nothing
#   ./test.sh --verbose    # show full gtest output, not just failures
#
# Run this before ./build_docker.sh: the docker build takes an hour, and a unit
# test failure here costs seconds.
#
# Requires bazel and a JDK -- run ./setup.sh first on a fresh machine.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd -P)"
cd "$SCRIPT_DIR"

# shellcheck source=jdk_detect.sh
source "$SCRIPT_DIR/jdk_detect.sh"

# The targets holding the tests this branch adds or touches.
TEST_TARGETS=(
  "//backend/storage:in_memory_storage_test"
  "//backend/schema/updater:schema_updater_test"
  "//frontend/handlers:queries_test"
  "//frontend/collections:session_manager_test"
)

VERBOSE=false
LIST=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --verbose) VERBOSE=true; shift ;;
    --list)    LIST=true; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *)         echo "unknown flag: $1" >&2; exit 1 ;;
  esac
done

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# The test cases added on this branch, grouped as they appear in the sources.
# Kept here so ./test.sh --list documents what the branch actually covers.
list_tests() {
  cat <<'EOF'

backend/storage/in_memory_storage_test.cc
  PurgeExpiredDeletedRows -- erasing rows whose tombstone has aged out
    PurgeRemovesRowDeletedBeforeRetentionWindow
    PurgeKeepsLiveRow
    PurgeKeepsRowDeletedInsideRetentionWindow
    PurgeKeepsResurrectedRow
    PurgeScopedToNamedTablesOnly
    PurgeIsIdempotent
    PurgeOnUnknownTableIsNoOp
    PurgeRemovesOnlyExpiredRowsFromMixedTable

  PurgeExpiredVersions -- trimming superseded versions of live rows
    PurgeVersionsTrimsSupersededVersionsOfLiveRow
    PurgeVersionsKeepsVersionsInsideRetentionWindow
    PurgeVersionsIsIdempotent

  PurgeWindow -- protecting seed data and the newest writes
    PurgeKeepsSeedRowDeletedInsideWindow
    PurgeRemovesRowCreatedAfterNotBefore
    PurgeKeepsRowDeletedInsideNotAfterLag
    PurgeVersionsKeepsSeedVersionOfLiveRow
    PurgeWindowLeavesRetentionBehaviourUnchanged

frontend/collections/session_manager_test.cc
  PruneSessionsNotUsedSince -- reclaiming abandoned sessions
    PruneRemovesSessionNotUsedSinceBound
    PruneKeepsRecentlyUsedSession
    PruneLeavesMultiplexedSessionsByDefault
    PruneIsIdempotent
    PruneRemovesOnlyTheSessionsPastTheBound

frontend/handlers/queries_test.cc
  EMULATOR_RECLAIM -- the statement, end to end over gRPC
    EmulatorReclaimReturnsCounters
    EmulatorReclaimAcceptsTableNames
    EmulatorReclaimAcceptsTableNameWithNamedArguments
    EmulatorReclaimAcceptsWindowBounds
    EmulatorReclaimRejectsUnparseableTimestamp
    EmulatorReclaimRejectsUnparseableDuration
    EmulatorReclaimRejectsInvertedWindow
    EmulatorReclaimRejectsUnboundedDeleteRows
    EmulatorReclaimAcceptsBoundedDeleteRows
    EmulatorReclaimKeepsRowsCommittedBeforeNotBefore

backend/schema/updater/schema_updater_test.cc
  Covers the DropIndex fix: an index's data table is now marked dropped so
  CleanUpDeletedTables() can reclaim it. Run whole; it is an upstream suite.

EOF
}

if [[ "$LIST" == true ]]; then
  list_tests
  exit 0
fi

if ! JAVA_HOME="$(detect_jdk)"; then
  echo "No JDK found. Bazel needs JAVA_HOME to point at a real JDK." >&2
  echo "Run ./setup.sh to diagnose, or install one:" >&2
  jdk_install_hint >&2
  exit 1
fi
export JAVA_HOME
log "JAVA_HOME=$JAVA_HOME"

test_output="errors"
[[ "$VERBOSE" == true ]] && test_output="all"

log "Running ${#TEST_TARGETS[@]} test target(s)"
list_tests

# --test_output=errors keeps a passing run quiet; --verbose shows everything.
bazel test "--test_output=${test_output}" "${TEST_TARGETS[@]}"

log "All reclamation tests passed. Safe to run ./build_docker.sh"
