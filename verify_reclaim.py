#!/usr/bin/env python3
"""Smoke test: does the emulator actually return memory after reclamation?

The engine fix and the allocator fix are independent, and a source-level test
cannot see the second one -- erasing from the storage maps returns nodes to the
allocator's free lists, not to the OS. So the only meaningful check is to watch
container RSS across a write / delete / reclaim cycle.

Usage:
    python3 verify_reclaim.py --port 19210 --container spanner-emulator-fork-verify
"""

import argparse
import json
import subprocess
import sys
import time
import urllib.request

PROJECT = "pj"
INSTANCE = "inst"
DATABASE = "reclaimdb"
ROWS = 20000
PAD = "x" * 400


def rss_mib(container):
    out = subprocess.run(
        ["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", container],
        capture_output=True, text=True, check=True).stdout
    value = out.split("/")[0].strip()
    if "GiB" in value:
        return float(value.replace("GiB", "")) * 1024
    return float(value.replace("MiB", ""))


def post(root, path, payload):
    request = urllib.request.Request(
        root + path, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    return json.load(urllib.request.urlopen(request, timeout=900))


def patch(root, path, payload):
    request = urllib.request.Request(
        root + path, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="PATCH")
    urllib.request.urlopen(request, timeout=900)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--container", required=True)
    args = parser.parse_args()

    root = f"http://localhost:{args.port}/v1/"
    databases = f"projects/{PROJECT}/instances/{INSTANCE}/databases"

    post(root, f"projects/{PROJECT}/instances", {
        "instanceId": INSTANCE,
        "instance": {
            "config": f"projects/{PROJECT}/instanceConfigs/emulator-config",
            "displayName": "verify", "nodeCount": 1}})
    post(root, databases, {
        "createStatement": f"CREATE DATABASE `{DATABASE}`",
        "extraStatements": [
            "CREATE TABLE T (id INT64 NOT NULL, a STRING(MAX), b STRING(MAX)) "
            "PRIMARY KEY (id)",
            "CREATE INDEX ix_a ON T(a)"]})

    # A short retention window so rows become purgeable within the test.
    patch(root, f"{databases}/{DATABASE}/ddl", {"statements": [
        f"ALTER DATABASE {DATABASE} SET OPTIONS (version_retention_period = '1s')"]})

    session = post(root, f"{databases}/{DATABASE}/sessions", {})["name"]
    baseline = rss_mib(args.container)
    print(f"  baseline                : {baseline:8.1f} MiB")

    for batch in range(ROWS // 250):
        post(root, f"{session}:commit", {
            "singleUseTransaction": {"readWrite": {}},
            "mutations": [{"insertOrUpdate": {
                "table": "T", "columns": ["id", "a", "b"],
                "values": [[str(batch * 250 + k), PAD, PAD] for k in range(250)]}}]})
    after_write = rss_mib(args.container)
    print(f"  after {ROWS} rows       : {after_write:8.1f} MiB  (+{after_write - baseline:.1f})")

    post(root, f"{session}:commit", {
        "singleUseTransaction": {"readWrite": {}},
        "mutations": [{"delete": {"table": "T", "keySet": {"all": True}}}]})
    after_delete = rss_mib(args.container)
    print(f"  after DELETE all        : {after_delete:8.1f} MiB  "
          f"({after_delete - after_write:+.1f})   <- tombstones, expect an increase")

    # Let the rows age past the 1s retention window before reclaiming.
    time.sleep(3)
    result = post(root, f"{session}:executeSql", {"sql": "SELECT EMULATOR_RECLAIM('T')"})
    purged = result.get("rows", [["?"]])[0][0]
    after_reclaim = rss_mib(args.container)
    print(f"  after EMULATOR_RECLAIM  : {after_reclaim:8.1f} MiB  "
          f"({after_reclaim - after_delete:+.1f})   rows purged: {purged}")

    remaining = post(root, f"{session}:executeSql", {"sql": "SELECT COUNT(*) FROM T"})
    print(f"  rows remaining          : {remaining['rows'][0][0]}")

    print()
    recovered = after_delete - after_reclaim
    growth = after_reclaim - baseline
    if recovered <= 0:
        print(f"  FAIL: reclaim returned no memory ({recovered:+.1f} MiB).")
        print("        The engine may be erasing correctly while the allocator")
        print("        holds the pages. Check that malloc_trim is compiled in.")
        return 1
    print(f"  PASS: recovered {recovered:.1f} MiB; {growth:.1f} MiB above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
