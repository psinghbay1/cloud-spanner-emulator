# Memory reclamation changes

This branch makes the emulator able to release memory held by deleted data,
instead of growing for the lifetime of the process.

Upstream is explicit that the emulator is *"specifically intended for local unit
testing"* and that *"all data is kept in memory and discarded when the emulator
terminates"*. That is fine for a short-lived process, but a CI machine that
keeps an emulator up across many test batches has no way to reclaim anything
short of restarting the container — which costs a full schema reload.

## What was wrong

Two independent problems. Fixing either alone changes nothing observable.

**1. The engine never erases deleted rows.** `InMemoryStorage::Delete()` appends
a tombstone (`_exists = false`, plus an invalid sentinel per column) at a new
timestamp. The row key is never removed — the header says so outright: *"Keys
are never deleted, but are marked deleted for multi-version lookup."*
`RemoveExpiredVersions()` can only trim a cell that is being written again, so a
row that is deleted and never touched again is never revisited.

**2. Freed memory never reaches the OS.** No custom allocator is linked, and
`malloc_trim` is never called. Erasing from `std::map` / `flat_hash_map` returns
nodes to glibc's per-arena free lists, so RSS does not drop even where the code
does erase correctly.

There is also a genuine bug: **`DropIndex` never marked its data table.**
`dropped_tables_` is written at exactly one site, inside `DropTable`. An index
keeps its rows in its own `index_data_table_` with its own `TableID`, so on
`DROP INDEX` that table left the schema graph but never reached
`MarkDroppedTable()` — its rows stayed in storage permanently, unreachable *and*
ineligible for cleanup.

## What changed

| Change | Files |
| --- | --- |
| `DROP INDEX` marks its data table for cleanup | `backend/schema/updater/schema_updater.cc` |
| `PurgeExpiredDeletedRows()` erases rows whose tombstone predates the retention window | `backend/storage/{storage.h,in_memory_storage.{h,cc}}` |
| `Database::ReclaimStorage()` runs every pass, then `malloc_trim(0)` | `backend/database/database.{h,cc}` |
| `SELECT EMULATOR_RECLAIM(...)` exposes it to clients | `frontend/handlers/queries.cc`, `frontend/entities/session.h` |
| Tests for the purge semantics and safety boundaries | `backend/storage/in_memory_storage_test.cc` |

### Why a SQL statement and not a REST endpoint

The REST gateway is generated from upstream googleapis protos
(`gateway/gateway.go` → `RegisterDatabaseAdminHandlerFromEndpoint`), and
`REGISTER_GRPC_HANDLER` binds handlers to generated proto message types. Adding
a `:reclaim` admin method would mean forking those protos and regenerating the
Go gateway — a large, permanent maintenance burden.

Intercepting a statement in `ExecuteSql` needs no proto change, no new port and
no client library. It works from `spanner-cli`, JDBC, or any existing session.

### Safety

A row is erased only when its deletion marker is older than
`timestamp - version_retention_period`, i.e. when it is invisible at every
timestamp a transaction may legally read. This is the same arithmetic
`CleanUpDeletedTables()` already uses. A row deleted inside the retention window,
or deleted and then re-inserted, is kept — both cases are covered by tests.

## Usage

```sql
-- Reclaim specific tables (also sweeps indexes defined on them)
SELECT EMULATOR_RECLAIM('Orders', 'LineItems');

-- Reclaim the whole database
SELECT EMULATOR_RECLAIM();
```

Returns a single `rows_purged` count. Idempotent; unknown table names are
ignored rather than rejected, so a harness sweeping a fixed list does not break
when the schema changes.

**This is emulator-only.** Cloud Spanner has no such statement and reclaims
storage in the background. Do not write production code against it.

## Build and verify

```bash
./build.sh                 # build //... then run the reclamation tests
./build.sh test            # tests only
./build.sh all-tests       # full upstream suite (slow)

./build_docker.sh          # build spanner-emulator-fork:latest
./build_docker.sh --verify # build, then prove RSS actually drops
```

`--verify` matters: a unit test cannot see problem 2. `verify_reclaim.py`
writes 20k rows, deletes them, calls `EMULATOR_RECLAIM`, and compares container
RSS before and after. If the engine erases correctly but the allocator holds the
pages, the unit tests still pass and this check fails.

## Status of the allocator fix

`malloc_trim(0)` is best effort — glibc can only return pages it can coalesce,
and a fragmented nested-map heap limits that. If `--verify` shows little
recovered, the next step is linking tcmalloc or jemalloc, which return spans far
more aggressively. **Confirm by measurement, not by reading the code.**
