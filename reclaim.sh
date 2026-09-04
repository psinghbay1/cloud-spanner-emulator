#!/usr/bin/env bash
#
# Trigger storage reclamation on a running emulator.
#
#   ./reclaim.sh mydb                         # sweep the whole database
#   ./reclaim.sh mydb Orders LineItems        # sweep only those tables
#   ./reclaim.sh --all                        # sweep every database on the emulator
#   ./reclaim.sh --list                       # list databases, do nothing
#   ./reclaim.sh --retention 1s mydb          # shorten the window first, then sweep
#
# Environment (all optional):
#   EMULATOR_REST   host:port of the REST gateway   (default localhost:9020)
#   PROJECT         project id                      (default bay1-pj-lab-eng)
#   INSTANCE        instance id                     (default local-spanner)
#
# EMULATOR ONLY. Cloud Spanner has no EMULATOR_RECLAIM statement; it reclaims
# storage in the background. This requires a build of the memory-reclamation
# fork -- the stock emulator rejects the statement.

set -euo pipefail

REST="${EMULATOR_REST:-localhost:9020}"
PROJECT="${PROJECT:-bay1-pj-lab-eng}"
INSTANCE="${INSTANCE:-local-spanner}"
BASE="http://${REST}/v1"
PREFIX="projects/${PROJECT}/instances/${INSTANCE}/databases"

RETENTION=""
ALL=false
LIST=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)       ALL=true; shift ;;
    --list)      LIST=true; shift ;;
    --retention) RETENTION="$2"; shift 2 ;;
    -h|--help)   sed -n '2,22p' "$0"; exit 0 ;;
    --*)         echo "unknown flag: $1" >&2; exit 1 ;;
    *)           break ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

api() {  # api METHOD PATH [JSON]
  local method="$1" path="$2" body="${3:-}"
  if [[ -n "$body" ]]; then
    curl -sS -X "$method" "${BASE}/${path}" -H 'Content-Type: application/json' -d "$body"
  else
    curl -sS -X "$method" "${BASE}/${path}"
  fi
}

list_databases() {
  api GET "$PREFIX" | python3 -c '
import json, sys
payload = json.load(sys.stdin)
for entry in payload.get("databases", []):
    print(entry["name"].rsplit("/", 1)[-1])'
}

# Reachability check up front: a connection error here is far clearer than a
# JSON parse failure three functions deep.
curl -sf "${BASE}/projects/${PROJECT}/instances" > /dev/null 2>&1 \
  || die "no emulator REST gateway at ${REST} (set EMULATOR_REST to override)"

if [[ "$LIST" == true ]]; then
  list_databases
  exit 0
fi

if [[ "$ALL" == true ]]; then
  mapfile -t DATABASES < <(list_databases)
  [[ ${#DATABASES[@]} -gt 0 ]] || die "no databases found on ${PROJECT}/${INSTANCE}"
  TABLES=()
else
  [[ $# -ge 1 ]] || die "usage: $0 [--all|--list] <database> [table ...]"
  DATABASES=("$1"); shift
  TABLES=("$@")
fi

total_rows=0
total_versions=0

for database in "${DATABASES[@]}"; do
  # Nothing older than version_retention_period is eligible, and the default is
  # one hour -- so on a fresh CI database a sweep correctly reports 0/0 unless
  # the window is shortened first.
  if [[ -n "$RETENTION" ]]; then
    api PATCH "${PREFIX}/${database}/ddl" \
      "{\"statements\":[\"ALTER DATABASE ${database} SET OPTIONS (version_retention_period = '${RETENTION}')\"]}" \
      > /dev/null || die "could not set retention on ${database}"
    sleep 2   # let the just-written rows age past the new window
  fi

  session="$(api POST "${PREFIX}/${database}/sessions" '{}' | python3 -c '
import json, sys
payload = json.load(sys.stdin)
if "name" not in payload:
    sys.exit("could not open a session: " + payload.get("message", str(payload)))
print(payload["name"])')" || die "database \"${database}\" not found on ${PROJECT}/${INSTANCE} (try --list)"

  args=""
  for table in ${TABLES+"${TABLES[@]}"}; do args+="${args:+, }'${table}'"; done

  response="$(api POST "${session}:executeSql" \
    "{\"sql\":\"SELECT EMULATOR_RECLAIM(${args})\"}")"

  # `read <<< $(...)` swallows the inner exit status, so capture first and check.
  parsed="$(printf '%s' "$response" | python3 -c '
import json, sys
payload = json.load(sys.stdin)
if "rows" not in payload:
    message = payload.get("message", str(payload))
    sys.exit("reclaim failed: " + message +
             "\n       (a stock emulator rejects EMULATOR_RECLAIM; this needs the fork build)")
row = payload["rows"][0]
print(row[0], row[1])')" || exit 1
  read -r rows versions <<< "$parsed"

  scope="whole database"
  [[ ${#TABLES[@]} -gt 0 ]] && scope="${#TABLES[@]} table(s)"
  printf '%-28s %10s rows  %10s versions   (%s)\n' "$database" "$rows" "$versions" "$scope"
  total_rows=$(( total_rows + rows ))
  total_versions=$(( total_versions + versions ))
done

if [[ ${#DATABASES[@]} -gt 1 ]]; then
  printf '%-28s %10s rows  %10s versions   (total)\n' "" "$total_rows" "$total_versions"
fi

cat <<EOF

Heap accounting is logged inside the container; RSS is the real measure:
  docker logs <container> 2>&1 | grep '\[reclaim\]'
  docker stats --no-stream --format '{{.MemUsage}}' <container>
EOF
