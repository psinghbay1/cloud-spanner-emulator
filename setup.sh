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

# --- 3. GNU readline (optional) -------------------------------------------
# Only ./build.sh all needs this: psql links against readline. The emulator
# binaries the release image ships do not, so a missing readline is not a
# failure -- just a note.
log "GNU readline (optional)"
if [[ "$(uname -s)" == "Linux" ]]; then
  if [[ -f /usr/include/readline/readline.h ]]; then
    ok "readline headers present"
  else
    warn "readline headers missing; only needed for ./build.sh all (psql)"
    warn "  sudo apt-get install -y libreadline-dev"
  fi
else
  ok "not required on $(uname -s)"
fi

# --- 4. Docker build DNS --------------------------------------------------
# The build network differs from `docker run`. On a host whose resolver is
# IPv6-only, build layers cannot reach it and every Bazel Go-dependency fetch
# fails with "no such host", ~20 minutes into the build.
log "Docker build-network DNS"
if ! command -v docker > /dev/null 2>&1; then
  warn "docker not found; skipping (only needed for build_docker.sh)"
else
  probe="$(printf 'FROM ubuntu:22.04\nRUN getent hosts storage.googleapis.com\n')"
  if printf '%s' "$probe" | docker build --no-cache -q - > /dev/null 2>&1; then
    ok "DNS resolves inside the build network"
  else
    warn "DNS FAILS inside the Docker build network"

    # Distinguish "daemon.json not loaded yet" from "the patch will not help".
    # A VPN/enterprise resolver can hand back private-range addresses and block
    # public DNS, in which case adding 8.8.8.8 achieves nothing.
    if grep -q '"dns"' "$HOME/.docker/daemon.json" 2>/dev/null; then
      warn "daemon.json already sets dns -- Docker Desktop has NOT been restarted yet"
      echo "       Restart Docker Desktop, then re-run: ./setup.sh --check"
      FAILED=true
      # Skip re-patching a file that is already correct.
      daemon=""
    else
      daemon="$HOME/.docker/daemon.json"
    fi

    if [[ -n "${daemon:-}" ]] && ! nc -z -w 3 8.8.8.8 53 > /dev/null 2>&1; then
      bad "8.8.8.8:53 is unreachable from this host (VPN or firewall?)."
      echo "       Adding public DNS to daemon.json will not help. Use your"
      echo "       corporate resolver's address instead, or build on a machine"
      echo "       without the restriction."
      FAILED=true
      daemon=""
    fi
    if [[ -z "${daemon:-}" ]]; then
      :  # Already diagnosed above; nothing further to patch.
    elif $CHECK_ONLY; then
      bad "re-run without --check to patch $daemon"
      FAILED=true
    else
      python3 - "$daemon" <<'PY'
import json, os, shutil, sys
path = sys.argv[1]
os.makedirs(os.path.dirname(path), exist_ok=True)
config = {}
if os.path.exists(path):
    shutil.copy(path, path + ".bak")
    try:
        config = json.load(open(path))
    except json.JSONDecodeError:
        config = {}
existing = config.get("dns") or []
for server in ("8.8.8.8", "1.1.1.1"):
    if server not in existing:
        existing.append(server)
config["dns"] = existing
json.dump(config, open(path, "w"), indent=2)
print(f"  patched {path} (backup at {path}.bak): dns={existing}")
PY
      cat <<'EOM'

  ACTION REQUIRED: restart Docker Desktop for this to take effect.
  Note that restarting stops any running containers.

  Then re-run: ./setup.sh --check
EOM
      FAILED=true
    fi
  fi
fi

# --- 5. Docker VM memory ---------------------------------------------------
# Compiling GoogleSQL includes tm_parser.cc, a bison-generated file that needs
# several GB in a single cc1plus process. Capping --jobs does not help: one
# process must fit. Docker Desktop defaults to a fraction of host RAM.
log "Docker VM memory"
if command -v docker > /dev/null 2>&1; then
  vm_bytes="$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)"
  vm_gib=$(( vm_bytes / 1024 / 1024 / 1024 ))
  host_gib=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1024 / 1024 / 1024 ))
  if (( vm_gib >= 24 )); then
    ok "${vm_gib} GiB available to Docker"
  elif (( vm_gib >= 16 )); then
    warn "${vm_gib} GiB available to Docker; the GoogleSQL build may still OOM"
    echo "       tm_parser.cc alone needs several GB in one process."
    echo "       Docker Desktop > Settings > Resources > Memory -> 24 GB or more."
    (( host_gib > 0 )) && echo "       This host has ${host_gib} GiB."
  else
    bad "${vm_gib} GiB available to Docker -- too small to build the emulator"
    echo "       Docker Desktop > Settings > Resources > Memory -> 24 GB or more."
    (( host_gib > 0 )) && echo "       This host has ${host_gib} GiB."
    FAILED=true
  fi
else
  warn "docker not found; skipping"
fi

log "Summary"
if $FAILED; then
  echo "  Some checks need attention (see FIX/FAIL above)."
  exit 1
fi
echo "  Ready. Next: ./build.sh   or   ./build_docker.sh"
