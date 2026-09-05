#!/usr/bin/env bash
#
# Locate a real JDK and print its home directory. Sourced by setup.sh and
# build.sh so both agree on what counts as a usable JDK.
#
# Bazel resolves @local_jdk from JAVA_HOME. A version-manager shim on PATH is
# not enough: without a real JDK directory the build fails during analysis with
# "no such target '@@rules_java~~toolchains~local_jdk//:bin/java'", which does
# not mention Java at all.


# A directory is a JDK home only if it has javac *and* a JDK marker file.
# /usr/bin/javac can be a shim whose parent (/usr) is not a JDK home at all --
# handing that to bazel produces the cryptic @local_jdk error this guards.
_is_jdk_home() {
  local dir="$1"
  [[ -n "$dir" && -x "$dir/bin/javac" ]] || return 1
  [[ -f "$dir/release" || -f "$dir/lib/modules" || -d "$dir/jmods" ]]
}

# Prints the JDK home on stdout and returns 0, or returns 1 if none is found.
# Never exits: callers decide how to report the failure. Every probe is guarded
# so a missing tool cannot trip `set -e` in the calling script.
detect_jdk() {
  local candidate

  if _is_jdk_home "${JAVA_HOME:-}"; then
    printf '%s' "$JAVA_HOME"
    return 0
  fi

  # macOS system locator. Absent on Linux, so test before calling it.
  if [[ -x /usr/libexec/java_home ]]; then
    candidate="$(/usr/libexec/java_home 2>/dev/null || true)"
    if _is_jdk_home "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  fi

  # Linux distro layouts: Debian/Ubuntu and RHEL/Fedora both use these.
  for candidate in /usr/lib/jvm/*; do
    _is_jdk_home "$candidate" || continue
    printf '%s' "$candidate"
    return 0
  done

  # javac on PATH: follow the symlink chain back to the JDK home.
  if command -v javac > /dev/null 2>&1; then
    candidate="$(command -v javac)"
    candidate="$(readlink -f "$candidate" 2>/dev/null || true)"
    candidate="${candidate%/bin/javac}"
    if _is_jdk_home "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  fi

  # Newest asdf- or sdkman-installed JDK.
  local root
  for root in "$HOME/.asdf/installs/java" "$HOME/.sdkman/candidates/java"; do
    [[ -d "$root" ]] || continue
    candidate="$(find "$root" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
    if _is_jdk_home "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  done

  return 1
}

# The install hint differs per platform, so keep it next to the detector.
jdk_install_hint() {
  if [[ "$(uname -s)" == "Linux" ]]; then
    echo "  sudo apt-get install -y openjdk-21-jdk      # Debian/Ubuntu"
    echo "  sudo dnf install -y java-21-openjdk-devel   # RHEL/Fedora"
  else
    echo "  asdf install java zulu-21.42.19"
  fi
}
