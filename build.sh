#!/usr/bin/env bash
#
# Build and test the emulator with the memory-reclamation changes.
#
#   ./build.sh              # build everything, then run the reclamation tests
#   ./build.sh test         # tests only (assumes a prior build)
#   ./build.sh all-tests    # the full upstream test suite (slow: 30-60+ min)
#
# Requires bazel (see README.md for the supported version) and a C++ toolchain.
# The first build compiles GoogleSQL and PostgreSQL sources and can take well
# over an hour; later builds are incremental.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd -P)"
cd "$SCRIPT_DIR"

MODE="${1:-build-and-test}"

# The targets covering the changes on this branch.
RECLAIM_TEST_TARGETS=(
  "//backend/storage:in_memory_storage_test"
  "//backend/schema/updater:schema_updater_test"
)

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

require_bazel() {
  if ! command -v bazel > /dev/null 2>&1; then
    echo "bazel not found on PATH. See README.md for installation." >&2
    exit 1
  fi
  log "bazel $(bazel --version 2>/dev/null | head -1)"
}

build_all() {
  log "Building //... (first run is slow; GoogleSQL + PostgreSQL are compiled from source)"
  bazel build //...
}

run_reclaim_tests() {
  log "Running memory-reclamation tests"
  # --test_output=errors keeps the log readable; switch to `all` to see the
  # full gtest output for a passing run.
  bazel test --test_output=errors "${RECLAIM_TEST_TARGETS[@]}"
}

run_all_tests() {
  log "Running the full test suite (this takes a long time)"
  bazel test --test_output=errors //...
}

require_bazel

case "$MODE" in
  build)
    build_all
    ;;
  test)
    run_reclaim_tests
    ;;
  all-tests)
    run_all_tests
    ;;
  build-and-test)
    build_all
    run_reclaim_tests
    ;;
  *)
    echo "usage: $0 [build|test|all-tests|build-and-test]" >&2
    exit 1
    ;;
esac

log "OK"