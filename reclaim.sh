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
# Seeded databases -- protect the seed data and the newest writes:
#   ./reclaim.sh --all --not-before "$SEED_DONE" --not-after-lag 60s
#
# --delete-rows additionally erases LIVE rows whose commit timestamp falls inside
# the window -- rows a test inserted and never deleted. The sweeps above only
# reclaim garbage, so on a seeded database this is what actually frees the bulk
# of the memory. It is destructive and requires a bound:
#   ./reclaim.sh --all --not-before "$SEED_DONE" --not-after-lag 60s --delete-rows
#
# --not-before protects everything written at or before that instant: seed data
# is the oldest data present, so a plain sweep reaches it first. --not-after-lag
# holds back the newest writes, so an in-flight test never loses rows underneath
# it. Together they purge only churn in between. Neither needs --retention: they
# bound the sweep directly, leaving version_retention_period (and therefore
# stale reads) alone.
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
NOT_BEFORE=""
NOT_AFTER_LAG=""
DELETE_ROWS=false
ALL=false
LIST=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)       ALL=true; shift ;;
    --list)      LIST=true; shift ;;
    --retention) RETENTION="$2"; shift 2 ;;
    --not-before) NOT_BEFORE="$2"; shift 2 ;;
    --not-after-lag) NOT_AFTER_LAG="$2"; shift 2 ;;
    --delete-rows) DELETE_ROWS=true; shift ;;
    -h|--help)   sed -n '2,33p' "$0"; exit 0 ;;
    --*)         echo "unknown flag: $1" >&2; exit 1 ;;
    *)           break ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

# Erasing live rows with no bound would empty the database. The emulator
# rejects this too; failing here gives a clearer message.
if [[ "$DELETE_ROWS" == true && -z "$NOT_BEFORE" && -z "$NOT_AFTER_LAG" ]]; then
  die "--delete-rows needs --not-before and/or --not-after-lag (refusing to erase every live row)"
fi

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
total_deleted=0

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
  if [[ -n "$NOT_BEFORE" ]]; then
    args+="${args:+, }not_before => '${NOT_BEFORE}'"
  fi
  if [[ -n "$NOT_AFTER_LAG" ]]; then
    args+="${args:+, }not_after_lag => '${NOT_AFTER_LAG}'"
  fi
  if [[ "$DELETE_ROWS" == true ]]; then
    args+="${args:+, }delete_rows => true"
  fi

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
print(row[0], row[1], row[2] if len(row) > 2 else 0)')" || exit 1
  read -r rows versions deleted <<< "$parsed"

  scope="whole database"
  [[ ${#TABLES[@]} -gt 0 ]] && scope="${#TABLES[@]} table(s)"
  printf '%-28s %10s rows  %10s versions  %10s deleted   (%s)\n' \
    "$database" "$rows" "$versions" "$deleted" "$scope"
  total_rows=$(( total_rows + rows ))
  total_versions=$(( total_versions + versions ))
  total_deleted=$(( total_deleted + deleted ))
done

if [[ ${#DATABASES[@]} -gt 1 ]]; then
  printf '%-28s %10s rows  %10s versions  %10s deleted   (total)\n' \
    "" "$total_rows" "$total_versions" "$total_deleted"
fi

cat <<EOF

Heap accounting is logged inside the container; RSS is the real measure:
  docker logs <container> 2>&1 | grep '\[reclaim\]'
  docker stats --no-stream --format '{{.MemUsage}}' <container>
EOF
