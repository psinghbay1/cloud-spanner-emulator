#!/usr/bin/env bash
#
# Build and test the emulator with the memory-reclamation changes.
#
#   ./build.sh              # build the emulator binaries, then run the reclamation tests
#   ./build.sh test         # tests only (assumes a prior build)
#   ./build.sh all          # every target, incl. developer tools (needs libreadline-dev)
#   ./build.sh all-tests    # the full upstream test suite (slow: 30-60+ min)
#   ./build.sh docker       # run the build+tests inside the upstream devcontainer
#
# NOTE: macOS cannot build this natively. The bundled PostgreSQL sources collide
# with macOS builtins (strlcat/strlcpy) and a gRPC layering_check fails. Both are
# pre-existing upstream issues, unrelated to the changes on this branch. The
# README states releases are built on Ubuntu with gcc. Use `./build.sh docker`,
# which runs the same commands inside the image the repo's .devcontainer uses.
#
# Run ./setup.sh first if this is a fresh machine -- it checks bazel, the JDK,
# and Docker build-network DNS.
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

# The targets the release image ships. build/docker/Dockerfile.ubuntu builds
# exactly these two and says so: "Build the emulator binaries only. Since we are
# only copying the binaries to the release image, we don't need to build the
# rest of the targets."
#
# Building //... instead pulls in developer tools the emulator never uses --
# notably //third_party/spanner_pg/src/bin/psql, which fails to link unless GNU
# readline development headers are installed. Use `./build.sh all` if you really
# want every target.
EMULATOR_TARGETS=(
  "//binaries:emulator_main"
  "//binaries:gateway_main"
)

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

require_bazel() {
  if ! command -v bazel > /dev/null 2>&1; then
    echo "bazel not found on PATH. See README.md for installation." >&2
    exit 1
  fi
  log "$(bazel --version 2>/dev/null | head -1)"
}

# shellcheck source=jdk_detect.sh
source "$SCRIPT_DIR/jdk_detect.sh"

require_jdk() {
  local jdk
  if ! jdk="$(detect_jdk)"; then
    echo "No JDK found. Bazel needs JAVA_HOME to point at a real JDK." >&2
    echo "Run ./setup.sh to diagnose, or install one:" >&2
    jdk_install_hint >&2
    exit 1
  fi
  export JAVA_HOME="$jdk"
  log "JAVA_HOME=$JAVA_HOME"
}

build_all() {
  log "Building the emulator binaries (first run is slow; GoogleSQL + PostgreSQL are compiled from source)"
  bazel build "${EMULATOR_TARGETS[@]}"
}

# Every target, including developer tools the emulator does not ship. Needs GNU
# readline installed for psql to link.
build_everything() {
  log "Building //... (includes developer tools; needs libreadline-dev for psql)"
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

# Run the build and tests inside the upstream devcontainer image, which has the
# Ubuntu/gcc toolchain the emulator actually supports. The bazel cache is kept in
# a named volume so repeat runs are incremental.
run_in_devcontainer() {
  local image="gcr.io/cloud-spanner-emulator/devcontainer"
  log "Running build + tests in $image"
  docker run --rm -it \
    -v "$SCRIPT_DIR:/workspace" \
    -v spanner-emulator-bazel-cache:/root/.cache/bazel \
    -w /workspace \
    "$image" \
    bash -lc "bazel test --test_output=errors ${RECLAIM_TEST_TARGETS[*]}"
}

if [[ "$MODE" != "docker" ]]; then
  require_bazel
  require_jdk
fi

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
  all)
    build_everything
    ;;
  docker)
    run_in_devcontainer
    ;;
  build-and-test)
    build_all
    run_reclaim_tests
    ;;
  *)
    echo "usage: $0 [build|test|all|all-tests|docker|build-and-test]" >&2
    exit 1
    ;;
esac

log "OK"