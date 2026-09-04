#!/usr/bin/env bash
#
# One-time host setup for building the emulator.
#
#   ./setup.sh            # check everything, fix what can be fixed
#   ./setup.sh --check    # report only, change nothing
#
# Checks:
#   1. bazel is installed and matches .bazelversion
#   2. JAVA_HOME points at a real JDK (bazel resolves @local_jdk from it)
#   3. DNS works inside the Docker BUILD network (a different network from
#      `docker run`; an IPv6-only host resolver breaks it)
#
# Everything here is a property of the machine, not of a build, which is why it
# lives in its own script rather than in build.sh or build_docker.sh.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd -P)"
cd "$SCRIPT_DIR"

CHECK_ONLY=false
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=true

log()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mOK\033[0m    %s\n' "$*"; }
warn() { printf '  \033[33mFIX\033[0m   %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; }

FAILED=false

# --- 1. bazel -------------------------------------------------------------
log "bazel"
if command -v bazel > /dev/null 2>&1; then
  have="$(bazel --version 2>/dev/null | awk '{print $2}')"
  want="$(cat .bazelversion 2>/dev/null || echo '')"
  if [[ -n "$want" && "$have" != "$want" ]]; then
    warn "bazel $have installed, .bazelversion wants $want (bazelisk handles this automatically)"
  else
    ok "bazel $have"
  fi
else
  bad "bazel not found. Install with: brew install bazelisk"
  FAILED=true
fi

# --- 2. JDK ---------------------------------------------------------------
# shellcheck source=jdk_detect.sh
source "$SCRIPT_DIR/jdk_detect.sh"

log "JDK"
if JDK_HOME="$(detect_jdk)"; then
  if [[ "${JAVA_HOME:-}" == "$JDK_HOME" ]]; then
    ok "JAVA_HOME=$JDK_HOME"
  else
    ok "found JDK at $JDK_HOME"
    warn "to use bazel directly, run: export JAVA_HOME=\"$JDK_HOME\""
  fi
else
  bad "No JDK found. Install one:"
  jdk_install_hint
  FAILED=true
fi
