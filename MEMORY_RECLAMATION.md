# Highnote fork — memory reclamation

This fork of [`GoogleCloudPlatform/cloud-spanner-emulator`](https://github.com/GoogleCloudPlatform/cloud-spanner-emulator)
adds the ability to **reclaim memory held by deleted data** at runtime, instead of
growing until the container is restarted.

Everything here concerns this repository only. Upstream behaviour is unchanged
unless a change is listed below.

## Table of contents

- [Why this fork exists](#why-this-fork-exists)
- [What changed](#what-changed)
- [Using it](#using-it)
- [Building](#building)
- [Repository layout](#repository-layout)
- [Design notes](#design-notes)
- [Status](#status)
- [Keeping the fork current](#keeping-the-fork-current)

## Why this fork exists

The upstream emulator never releases memory for deleted data. A container that
stays up across many test batches grows for its whole lifetime and is eventually
OOM-killed **silently** — zero error output, and the client retries `UNAVAILABLE`
forever, so it presents as a long hang rather than a failure.

Measured against `emulator:1.5.55`, with the data verified gone afterwards in
every case:

| Attempt | Before | After | Result |
| --- | ---: | ---: | --- |
| Mutation `delete`, `keySet: all` (30k rows) | 155 MiB | 180 MiB | **+25 MiB** |
| DML `DELETE FROM t WHERE true` (10k rows) | 46.5 MiB | 49.1 MiB | **+2.6 MiB** |
| `DROP TABLE` × 10 | 45.9 MiB | 45.5 MiB | no reclaim |
| `DROP DATABASE` | 45.0 MiB | 45.3 MiB | no reclaim |
| `DELETE INSTANCE` | 45.3 MiB | 45.5 MiB | no reclaim |
| `DROP INDEX` × 142 (real schema) | 856.6 MiB | 858.7 MiB | no reclaim |
| **Restart container** | any | **~13–15 MiB** | **full reclaim** |

Deleting data makes memory go *up*. Only restarting the process returns it.

### Root cause — two independent problems

**1. The engine never erases deleted rows.** `InMemoryStorage::Delete()` appends a
tombstone (`_exists = false`, plus an invalid sentinel per column) at a new
timestamp and leaves the row key in place. The header states it outright:
*"Keys are never deleted, but are marked deleted for multi-version lookup."*
`RemoveExpiredVersions()` only trims a cell **while that cell is being written**,
so a row deleted — or updated — and then left alone is never revisited.

**2. Freed memory never reaches the OS.** No custom allocator is linked and
`malloc_trim` is never called, so erasing from `std::map` / `flat_hash_map`
returns nodes to glibc's free lists and RSS does not move.

Fixing either alone changes nothing observable.

> This is not a criticism of upstream. The README is explicit that the emulator is
> *"specifically intended for local unit testing"* and that *"all data is kept in
> memory and discarded when the emulator terminates"*. Long-lived containers are
> outside its design envelope. This fork extends that envelope.

## What changed

| Change | Files |
| --- | --- |
| `PurgeExpiredDeletedRows()` — erases rows whose tombstone predates the retention window, key included | `backend/storage/{storage.h, in_memory_storage.{h,cc}}` |
| `PurgeExpiredVersions()` — erases superseded versions of **live** rows | `backend/storage/{storage.h, in_memory_storage.{h,cc}}` |
| `Database::ReclaimStorage()` — runs every pass, then `malloc_trim(0)` | `backend/database/database.{h,cc}` |
| `SELECT EMULATOR_RECLAIM(...)` — client-facing trigger | `frontend/handlers/queries.cc`, `frontend/entities/session.h` |
| **Bug fix:** `DROP INDEX` now marks its data table for cleanup | `backend/schema/updater/schema_updater.cc` |
| 14 tests covering purge semantics and safety boundaries | `backend/storage/in_memory_storage_test.cc` |
| `BAZEL_JOBS` build arg to cap parallelism | `build/docker/Dockerfile.ubuntu` |
| `setup.sh`, `build.sh`, `build_docker.sh`, `verify_reclaim.py` | repo root |

Additive only — no upstream behaviour is altered.

### The `DROP INDEX` bug

Worth calling out separately, because it is a genuine upstream defect rather than
a new feature.

`dropped_tables_` is written at **exactly one site**, inside `DropTable`. An index
keeps its rows in its own `index_data_table_` with its own `TableID`, but
`DropIndex` called only `DropNode(index)`. So on `DROP INDEX` the data table left
the schema graph while its rows stayed in storage — unreachable by schema **and**
ineligible for cleanup, permanently.

```
DROP TABLE  ─▶ dropped_tables_.push_back(id) ─▶ MarkDroppedTable ─▶ CleanUp ─▶ erase ✅
DROP INDEX  ─▶ DropNode(index) only          ─▶ (nothing)        ─▶ never   ─▶ leak ❌
```

The fix is ~4 lines and is the strongest candidate to send upstream.

## Using it

```sql
SELECT EMULATOR_RECLAIM('Orders', 'LineItems');  -- scoped; also sweeps their indexes
SELECT EMULATOR_RECLAIM();                       -- whole database
```

Returns one row:

| Column | Meaning |
| --- | --- |
| `rows_purged` | deleted rows whose tombstone predated the retention window |
| `versions_purged` | superseded versions of rows that are still live |

**Set a short retention window first**, or nothing recent is eligible:

```sql
ALTER DATABASE mydb SET OPTIONS (version_retention_period = '1s');
```

The default is **1 hour**. The emulator explicitly permits sub-hour values
(logging that this is emulator-only). Without this you will correctly get `0 / 0`
for anything written in the last hour.

**There is no scheduler.** Nothing runs on a timer — the emulator's only
background thread is the change-stream partition churner. Call the statement from
wherever your harness has a natural boundary.

Any client that can run a query works: `spanner-cli`, JDBC, the official client
libraries, or plain `curl` against the REST gateway on port 9020.

**Emulator-only.** Cloud Spanner has no such statement; it reclaims storage in the
background. Never write production code against it.

## Building

```bash
./setup.sh          # one-time host checks: bazel, JDK, Docker build DNS
./build.sh          # build //... then run the reclamation tests
./build.sh docker   # same, inside the upstream devcontainer image
./build_docker.sh --tag spanner-emulator-reclaim:dev --verify
```

`build.sh` modes: `build`, `test`, `all-tests`, `docker`, or no argument for
build-then-test.

### Why `setup.sh` exists

Two environment problems fail in ways that never name their cause:

- **`JAVA_HOME` unset.** Bazel resolves `@local_jdk` from it; a version-manager
  shim on `PATH` is not enough. Without a real JDK directory the build fails
  during analysis with `no such target
  '@@rules_java~~toolchains~local_jdk//:bin/java'`.
- **DNS inside the Docker *build* network.** That network differs from
  `docker run`, and on a host with an IPv6-only resolver the build layers cannot
  reach it. Bazel's Go dependency fetches fail with `no such host` roughly 20
  minutes in, while `docker run` containers resolve fine.

### Why `BAZEL_JOBS` exists

`.bazelrc` sets `--jobs=auto`, which sizes parallelism to CPU count and ignores
memory. Each `cc1plus` compiling GoogleSQL needs roughly 1–2 GB, so a Docker VM
with less RAM than *(cores × 2 GB)* dies with:

```
gcc: fatal error: Killed signal terminated program cc1plus
ResourceExhausted: cannot allocate memory
```

`build_docker.sh` reads the VM's actual memory and CPU count and budgets one job
per 2 GB. Override with `--jobs N` or `BAZEL_JOBS=N`.

### Platform support

macOS **cannot** build this natively — two pre-existing upstream issues block it,
both unrelated to this fork:

1. Bundled PostgreSQL vs macOS builtins:
   `port.h:452: conflicting types for '__builtin___strlcat_chk'`
2. A gRPC layering check on `absl/synchronization/mutex.h`

Upstream's README states releases are built on Ubuntu with gcc. Use
`./build.sh docker` or `./build_docker.sh`.

## Repository layout

Only the files this fork adds or changes:

```
MEMORY_RECLAMATION.md        this file
setup.sh                     one-time host setup
build.sh                     build + test (native or devcontainer)
build_docker.sh              build the container image
verify_reclaim.py            end-to-end RSS check

backend/storage/
  storage.h                  + PurgeExpiredDeletedRows, PurgeExpiredVersions
  in_memory_storage.{h,cc}   implementations
  in_memory_storage_test.cc  + 14 tests

backend/database/
  database.{h,cc}            + ReclaimStorage(), malloc_trim

backend/schema/updater/
  schema_updater.cc          DROP INDEX marks its data table

frontend/
  handlers/queries.cc        EMULATOR_RECLAIM interception
  entities/session.h         + database() accessor

build/docker/Dockerfile.ubuntu   + BAZEL_JOBS build arg
```

## Design notes

### Why a SQL statement rather than a REST endpoint

The REST gateway is generated from upstream googleapis protos
(`gateway/gateway.go` → `RegisterDatabaseAdminHandlerFromEndpoint`), and
`REGISTER_GRPC_HANDLER` binds handlers to generated proto message types. A
`:reclaim` admin method would require forking those protos and regenerating the Go
gateway — a permanent maintenance burden on every upstream bump.

Intercepting a statement in `ExecuteSql` needs no proto change, no new port, and
no client library.

### How the interception works

`EMULATOR_RECLAIM` is **not real SQL**. `IsEmulatorReclaimStatement` matches a
whole-statement prefix, case-insensitively, in `ExecuteSql` — **before**
`FindOrInitTransaction` and `GuardedCall`, because reclamation is not a query and
takes no transaction.

A real SQL function could not do this: GoogleSQL functions are evaluated inside a
transaction against a snapshot and cannot erase rows from under the engine.

Matching only a whole-statement prefix means it cannot collide with a schema — 
`SELECT emulator_reclaim FROM T` is unaffected.

`ParseReclaimTableNames` extracts single-quoted names; an empty list means "every
table". The result set is synthesised by hand, with values set via
`set_string_value` because the Spanner wire protocol encodes `INT64` as a string.

### Safety

A row is erased only when its deletion marker is older than
`timestamp - version_retention_period` — invisible at every timestamp a
transaction may legally read. This is the same arithmetic `CleanUpDeletedTables()`
already uses.

Covered by tests: rows deleted inside the retention window are kept; rows deleted
then re-inserted are kept; scoped sweeps touch only the named tables; both purges
are idempotent.

## Status

| Item | State |
| --- | --- |
| Code complete | yes |
| `backend/storage` compiles | **verified** |
| 14 unit tests | written, **not yet executed** |
| `malloc_trim` returns memory to the OS | **unproven** |
| Container RSS measured before/after reclaim | **not done** |

**The `malloc_trim` half is the one that matters and is not yet proven.** A unit
test cannot observe it: the engine can erase correctly while glibc holds the
pages, in which case the purge counts look right and RSS does not move. Only
`verify_reclaim.py` — or an equivalent container measurement — settles it. If it
shows little recovered, the next step is linking tcmalloc or jemalloc.

### Known gaps

- **Change streams were not analysed.** They retain their own partition data and
  own the only background thread in the backend. Whether they pin deleted rows is
  unexamined and could affect `PurgeExpiredDeletedRows`.
- **Purge cost is unmeasured.** Both purges walk every key in the targeted tables
  under the storage mutex, which could stall concurrent transactions on a large
  table.
- **Interleaved child rows** whose parent is purged are untested.
- **Dropped databases** are not swept — the whole `Database` object is destroyed,
  so there is nothing left to reclaim.

## Keeping the fork current

The diff is deliberately small and additive so rebasing onto upstream stays cheap.

The `DROP INDEX` fix (`schema_updater.cc`) is an unambiguous upstream defect and
should be sent as its own PR. `malloc_trim` is one guarded line and would benefit
every emulator user. Landing both upstream would leave this fork carrying only the
purge methods and the SQL trigger.
