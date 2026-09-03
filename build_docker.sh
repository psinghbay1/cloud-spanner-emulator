#!/usr/bin/env bash
#
# Build a local Docker image of the emulator with the memory-reclamation
# changes, and optionally smoke-test it.
#
#   ./build_docker.sh                    # build spanner-emulator-fork:latest
#   ./build_docker.sh --tag my:tag       # build with a custom tag
#   ./build_docker.sh --verify           # build, then run a reclamation smoke test
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tag) IMAGE_TAG="$2"; shift 2 ;;
    --verify) VERIFY=true; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 1 ;;
  esac
done

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

log "Building $IMAGE_TAG (this takes a while on a cold cache)"
docker build . -t "$IMAGE_TAG" -f build/docker/Dockerfile.ubuntu

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
