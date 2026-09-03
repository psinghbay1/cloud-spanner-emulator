#!/usr/bin/env bash
#
# Build a local Docker image of the emulator with the memory-reclamation
# changes, and optionally smoke-test it.
#
#   ./build_docker.sh                    # build spanner-emulator-fork:latest
#   ./build_docker.sh --tag my:tag       # build with a custom tag
#   ./build_docker.sh --verify           # build, then run a reclamation smoke test
#   ./build_docker.sh --low-memory       # aggressive GCC GC; use if the build OOMs
#   ./build_docker.sh --jobs 4           # cap bazel parallelism by hand
#
# The build compiles the emulator from source inside the container and takes a
# long time on a cold cache (typically 45-90 min). Docker layer caching makes
# later builds much faster as long as the base stages are unchanged.
#
# NOTE ON ARCHITECTURE: this builds for the host architecture. bayone's
# configure_spanner_emulator.sh pulls gcr.io on arm64 and the internal
# us-docker.pkg.dev mirror on amd64, so an image built on an arm64 laptop will
# NOT be used by amd64 CI. To ship this to CI it must be built for
# linux/amd64 and pushed to that mirror.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd -P)"
cd "$SCRIPT_DIR"

IMAGE_TAG="spanner-emulator-fork:latest"
VERIFY=false
LOW_MEMORY=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tag) IMAGE_TAG="$2"; shift 2 ;;
    --jobs) BAZEL_JOBS="$2"; shift 2 ;;
    --low-memory) LOW_MEMORY=true; shift ;;
    --verify) VERIFY=true; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 1 ;;
  esac
done

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Bazel fetches Go dependencies during the build, so the BUILD network must be
# able to resolve names. That is a different network from `docker run`, and on a
# host whose resolver is IPv6-only the build layers cannot reach it: every fetch
# fails with "no such host" roughly 20 minutes in. Fail fast instead, and point
# at setup.sh, which fixes it once for the machine.
check_build_dns() {
  log "Checking DNS inside the Docker build network"
  if printf 'FROM ubuntu:22.04\nRUN getent hosts storage.googleapis.com\n' \
       | docker build --no-cache -q - > /dev/null 2>&1; then
    echo "  OK"
    return
  fi
  cat >&2 <<'EOM'

ERROR: DNS does not resolve inside the Docker build network, so Bazel cannot
fetch its Go dependencies. This is a host configuration issue, not a problem
with this branch.

Run the one-time host setup, then restart Docker Desktop:

  ./setup.sh

EOM
  exit 1
}

check_build_dns

# .bazelrc sets --jobs=auto, which sizes parallelism to CPU count and ignores
# memory. Each cc1plus compiling GoogleSQL needs roughly 1-2 GB, so when the
# Docker VM has less RAM than (cores x 2 GB) the build is OOM-killed:
#   gcc: fatal error: Killed signal terminated program cc1plus
#   ResourceExhausted: cannot allocate memory
# Budget one job per 2 GB of VM memory, floor 1, and never exceed the VM's CPUs.
compute_bazel_jobs() {
  if [[ -n "${BAZEL_JOBS:-}" ]]; then
    log "Using BAZEL_JOBS=$BAZEL_JOBS (from environment)"
    return
  fi
  local mem_bytes cpus by_mem
  mem_bytes="$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)"
  cpus="$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)"
  if [[ "$mem_bytes" -le 0 || "$cpus" -le 0 ]]; then
    log "Could not read Docker VM resources; leaving bazel parallelism at default"
    return
  fi
  # GoogleSQL's tm_parser.cc is a bison-generated file whose single cc1plus
  # process peaks at roughly 8-10 GB. Budgeting one job per 2 GB is not enough:
  # at --jobs=15 on a 31 GiB VM the build still died on that file, because the
  # other 15 jobs had already claimed the memory it needed. Budget 4 GB per job
  # so the peak file has room even when the rest of the build is saturated.
  by_mem=$(( mem_bytes / (4 * 1024 * 1024 * 1024) ))
  (( by_mem < 1 )) && by_mem=1
  BAZEL_JOBS=$(( by_mem < cpus ? by_mem : cpus ))
  log "Docker VM: $((mem_bytes / 1024 / 1024 / 1024)) GiB / ${cpus} CPUs -> --jobs=$BAZEL_JOBS (4 GB/job)"
  if (( BAZEL_JOBS < 4 )); then
    echo "  NOTE: this is low and the build will be slow. Raise Docker Desktop's"
    echo "        memory limit (Settings > Resources) for a faster build."
  fi
}

compute_bazel_jobs

BUILD_ARGS=()
[[ -n "${BAZEL_JOBS:-}" ]] && BUILD_ARGS+=(--build-arg "BAZEL_JOBS=$BAZEL_JOBS")

# --low-memory makes GCC collect far more aggressively while compiling. Peak RSS
# on the heavy generated files drops noticeably at maybe 10-20% more compile
# time, and unlike lowering -O it does not change the generated code.
if [[ "$LOW_MEMORY" == true ]]; then
  GCC_MEMORY_COPTS="--copt=--param=ggc-min-expand=10 --copt=--param=ggc-min-heapsize=32768 --copt=-fno-var-tracking --copt=-fno-var-tracking-assignments"
  log "Low-memory mode: aggressive GCC garbage collection"
  BUILD_ARGS+=(--build-arg "GCC_MEMORY_COPTS=$GCC_MEMORY_COPTS")
fi

log "Building $IMAGE_TAG (this takes a while on a cold cache)"
docker build "${BUILD_ARGS[@]}" . -t "$IMAGE_TAG" -f build/docker/Dockerfile.ubuntu

log "Built $IMAGE_TAG"
docker images --format '  {{.Repository}}:{{.Tag}}  {{.Size}}' | grep -F "${IMAGE_TAG%%:*}" || true

cat <<EOF

Run it locally:

  docker run -d --rm --name spanner-emulator-fork \\
    -p 9010:9010 -p 9020:9020 -m 3g "$IMAGE_TAG"

  export SPANNER_EMULATOR_HOST=localhost:9010

Point bayone's local CI at it by retagging to the pinned name in
configure_spanner_emulator.sh:16 (EMULATOR_VERSION), or by overriding
EMULATOR_URL for a one-off run.
EOF

if [[ "$VERIFY" != true ]]; then
  exit 0
fi

log "Smoke test: does reclamation actually return memory?"

CONTAINER="spanner-emulator-fork-verify"
docker rm -f "$CONTAINER" > /dev/null 2>&1 || true
docker run -d --rm --name "$CONTAINER" -p 19210:9020 -m 3g "$IMAGE_TAG" > /dev/null

cleanup() { docker rm -f "$CONTAINER" > /dev/null 2>&1 || true; }
trap cleanup EXIT

# Wait for the REST port rather than sleeping a fixed amount.
for _ in $(seq 1 60); do
  if curl -sf "http://localhost:19210/v1/projects/pj/instances" > /dev/null 2>&1; then
    break
  fi
  sleep 1
done

python3 "$SCRIPT_DIR/verify_reclaim.py" --port 19210 --container "$CONTAINER"
