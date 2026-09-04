#!/usr/bin/env python3
"""Reproduce the seven memory scenarios, optionally with EMULATOR_RECLAIM.

Runs the same sequence used to characterise the upstream emulator, so the
numbers are directly comparable:

    1. mutation delete, keySet: all      (30k rows)
    2. DML DELETE FROM t WHERE true      (10k rows)
    3. DROP TABLE x10
    4. DROP DATABASE
    5. DELETE INSTANCE
    6. DROP INDEX x142                   (synthetic, or a real schema file)
    7. restart container

With --reclaim, EMULATOR_RECLAIM is called after each destructive step, which is
only meaningful against a build of this fork.

Usage:
    python3 benchmark_reclaim.py --image gcr.io/cloud-spanner-emulator/emulator:1.5.55
    python3 benchmark_reclaim.py --image spanner-emulator-reclaim:dev --reclaim
"""

import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

PROJECT = "pj"
INSTANCE = "inst"
PAD = "x" * 400
CONTAINER = "reclaim-benchmark"


def sh(*args, check=False):
    return subprocess.run(args, capture_output=True, text=True, check=check)


def _to_mib(value):
    value = value.strip()
    if not value:
        return float("nan")
    if "GiB" in value:
        return float(value.replace("GiB", "")) * 1024
    if "KiB" in value:
        return float(value.replace("KiB", "")) / 1024
    if "B" == value[-1] and "iB" not in value:
        return float(value[:-1]) / (1024 * 1024)
    return float(value.replace("MiB", ""))


def resources(container):
    """RSS, memory-limit share and CPU for the container, in one docker call."""
    out = sh("docker", "stats", "--no-stream", "--format",
             "{{.MemUsage}}|{{.MemPerc}}|{{.CPUPerc}}|{{.PIDs}}", container).stdout.strip()
    if not out:
        return {"rss": float("nan"), "mem_pct": float("nan"),
                "cpu_pct": float("nan"), "pids": 0}
    usage, mem_pct, cpu_pct, pids = out.split("|")
    return {
        "rss": _to_mib(usage.split("/")[0]),
        "limit": _to_mib(usage.split("/")[1]),
        "mem_pct": float(mem_pct.rstrip("%")),
        "cpu_pct": float(cpu_pct.rstrip("%")),
        "pids": int(pids),
    }


def rss(container):
    return resources(container)["rss"]


class Emulator:
    def __init__(self, image, port=19400):
        self.image, self.port = image, port
        self.base = f"http://localhost:{port}/v1"

    def start(self):
        sh("docker", "rm", "-f", CONTAINER)
        sh("docker", "run", "-d", "--rm", "--name", CONTAINER,
           "-m", "3g", "-p", f"{self.port}:9020", self.image, check=True)
        for _ in range(90):
            try:
                urllib.request.urlopen(f"{self.base}/projects/{PROJECT}/instances", timeout=2)
                return
            except Exception:
                time.sleep(1)
        raise RuntimeError("emulator did not become ready")

    def stop(self):
        sh("docker", "rm", "-f", CONTAINER)

    def call(self, path, payload, method="POST"):
        request = urllib.request.Request(
            f"{self.base}/{path}", data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"}, method=method)
        return json.load(urllib.request.urlopen(request, timeout=900))

    def delete(self, path):
        request = urllib.request.Request(f"{self.base}/{path}", method="DELETE")
        urllib.request.urlopen(request, timeout=300)

    def create_instance(self):
        self.call(f"projects/{PROJECT}/instances", {
            "instanceId": INSTANCE,
            "instance": {"config": f"projects/{PROJECT}/instanceConfigs/emulator-config",
                         "displayName": "bench", "nodeCount": 1}})

    def create_database(self, name, statements):
        self.call(f"projects/{PROJECT}/instances/{INSTANCE}/databases",
                  {"createStatement": f"CREATE DATABASE `{name}`",
                   "extraStatements": statements})

    def ddl(self, database, statements):
        self.call(f"projects/{PROJECT}/instances/{INSTANCE}/databases/{database}/ddl",
                  {"statements": statements}, method="PATCH")

    def session(self, database):
        return self.call(
            f"projects/{PROJECT}/instances/{INSTANCE}/databases/{database}/sessions", {})["name"]

    def reclaim(self, session, tables=()):
        args = ", ".join(f"'{t}'" for t in tables)
        try:
            result = self.call(f"{session}:executeSql",
                               {"sql": f"SELECT EMULATOR_RECLAIM({args})"})
            return tuple(result["rows"][0])
        except urllib.error.HTTPError as error:
            return ("n/a", error.read()[:60].decode(errors="replace"))


RESULTS = []
PEAK = {"rss": 0.0, "cpu": 0.0}


def record(name, before, after, note="", stats=None):
    delta = after - before
    if delta < -1:
        verdict = f"RECLAIMED {-delta:.1f} MiB"
    elif delta > 1:
        verdict = f"+{delta:.1f} MiB"
    else:
        verdict = "no change"
    pct = stats["mem_pct"] if stats else float("nan")
    cpu = stats["cpu_pct"] if stats else float("nan")
    PEAK["rss"] = max(PEAK["rss"], before, after)
    PEAK["cpu"] = max(PEAK["cpu"], 0.0 if cpu != cpu else cpu)
    RESULTS.append((name, before, after, verdict, note, pct, cpu))
    print(f"  {name:42} {before:7.1f} -> {after:7.1f} MiB  "
          f"[{pct:5.1f}% of cap, cpu {cpu:5.1f}%]   {verdict} {note}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--reclaim", action="store_true",
                        help="call EMULATOR_RECLAIM after each step (fork builds only)")
    parser.add_argument("--retention", default="1s")
    args = parser.parse_args()

    emulator = Emulator(args.image)
    print(f"\nImage: {args.image}   reclaim={'on' if args.reclaim else 'off'}\n")
    emulator.start()
    try:
        emulator.create_instance()
        baseline = rss(CONTAINER)
        print(f"  {'fresh container':42} {baseline:7.1f} MiB (baseline)\n")

        # --- 1. mutation delete, 30k rows ---------------------------------
        emulator.create_database("benchone", [
            "CREATE TABLE T (id INT64 NOT NULL, a STRING(MAX), b STRING(MAX)) PRIMARY KEY (id)"])
        emulator.ddl("benchone", [
            f"ALTER DATABASE benchone SET OPTIONS (version_retention_period = '{args.retention}')"])
        session = emulator.session("benchone")
        for batch in range(120):
            emulator.call(f"{session}:commit", {
                "singleUseTransaction": {"readWrite": {}},
                "mutations": [{"insertOrUpdate": {
                    "table": "T", "columns": ["id", "a", "b"],
                    "values": [[str(batch * 250 + k), PAD, PAD] for k in range(250)]}}]})
        before = rss(CONTAINER)
        emulator.call(f"{session}:commit", {
            "singleUseTransaction": {"readWrite": {}},
            "mutations": [{"delete": {"table": "T", "keySet": {"all": True}}}]})
        note = ""
        if args.reclaim:
            time.sleep(3)
            note = "purged=%s/%s" % emulator.reclaim(session, ["T"])
        record("1. mutation delete, keySet all (30k)", before, rss(CONTAINER), note, resources(CONTAINER))

        # --- 2. DML delete, 10k rows --------------------------------------
        emulator.create_database("benchtwo", [
            "CREATE TABLE T (id INT64 NOT NULL, a STRING(MAX)) PRIMARY KEY (id)"])
        emulator.ddl("benchtwo", [
            f"ALTER DATABASE benchtwo SET OPTIONS (version_retention_period = '{args.retention}')"])
        session2 = emulator.session("benchtwo")
        for batch in range(40):
            emulator.call(f"{session2}:commit", {
                "singleUseTransaction": {"readWrite": {}},
                "mutations": [{"insertOrUpdate": {
                    "table": "T", "columns": ["id", "a"],
                    "values": [[str(batch * 250 + k), PAD] for k in range(250)]}}]})
        before = rss(CONTAINER)
        transaction = emulator.call(f"{session2}:beginTransaction",
                                    {"options": {"readWrite": {}}})["id"]
        emulator.call(f"{session2}:executeSql",
                      {"sql": "DELETE FROM T WHERE true", "transaction": {"id": transaction}})
        emulator.call(f"{session2}:commit", {"transactionId": transaction})
        note = ""
        if args.reclaim:
            time.sleep(3)
            note = "purged=%s/%s" % emulator.reclaim(session2, ["T"])
        record("2. DML DELETE FROM t WHERE true (10k)", before, rss(CONTAINER), note, resources(CONTAINER))

        # --- 3. DROP TABLE x10 --------------------------------------------
        emulator.create_database("benchthree", [
            f"CREATE TABLE T{i} (id INT64 NOT NULL, a STRING(MAX)) PRIMARY KEY (id)"
            for i in range(10)])
        emulator.ddl("benchthree", [
            f"ALTER DATABASE benchthree SET OPTIONS (version_retention_period = '{args.retention}')"])
        session3 = emulator.session("benchthree")
        for table in range(10):
            for batch in range(4):
                emulator.call(f"{session3}:commit", {
                    "singleUseTransaction": {"readWrite": {}},
                    "mutations": [{"insertOrUpdate": {
                        "table": f"T{table}", "columns": ["id", "a"],
                        "values": [[str(batch * 250 + k), PAD] for k in range(250)]}}]})
        before = rss(CONTAINER)
        emulator.ddl("benchthree", [f"DROP TABLE T{i}" for i in range(10)])
        time.sleep(3)
        note = ""
        if args.reclaim:
            note = "purged=%s/%s" % emulator.reclaim(session3)
        record("3. DROP TABLE x10", before, rss(CONTAINER), note, resources(CONTAINER))

        # --- 4. DROP DATABASE ---------------------------------------------
        before = rss(CONTAINER)
        emulator.delete(f"projects/{PROJECT}/instances/{INSTANCE}/databases/benchthree")
        time.sleep(5)
        record("4. DROP DATABASE", before, rss(CONTAINER), "", resources(CONTAINER))

        # --- 6. DROP INDEX x142 (before deleting the instance) -------------
        # 142 indexes, split across two tables: Spanner caps a table at 128
        # indexes, and repeating a (table, column) pair is rejected, so each
        # index gets its own column.
        statements = []
        for table, count in (("W1", 100), ("W2", 42)):
            columns = ", ".join(f"c{i} STRING(MAX)" for i in range(count))
            statements.append(
                f"CREATE TABLE {table} (id INT64 NOT NULL, {columns}) PRIMARY KEY (id)")
        statements += [f"CREATE INDEX ixa{i} ON W1(c{i})" for i in range(100)]
        statements += [f"CREATE INDEX ixb{i} ON W2(c{i})" for i in range(42)]
        emulator.create_database("benchidx", statements)
        emulator.ddl("benchidx", [
            f"ALTER DATABASE benchidx SET OPTIONS (version_retention_period = '{args.retention}')"])
        session4 = emulator.session("benchidx")
        before = rss(CONTAINER)
        emulator.ddl("benchidx",
                     [f"DROP INDEX ixa{i}" for i in range(100)]
                     + [f"DROP INDEX ixb{i}" for i in range(42)])
        time.sleep(4)
        note = ""
        if args.reclaim:
            note = "purged=%s/%s" % emulator.reclaim(session4)
        record("6. DROP INDEX x142", before, rss(CONTAINER), note, resources(CONTAINER))

        # --- 5. DELETE INSTANCE -------------------------------------------
        before = rss(CONTAINER)
        emulator.delete(f"projects/{PROJECT}/instances/{INSTANCE}")
        time.sleep(6)
        record("5. DELETE INSTANCE", before, rss(CONTAINER), "", resources(CONTAINER))

        # --- 7. restart container -----------------------------------------
        before = rss(CONTAINER)
        emulator.stop()
        emulator.start()
        record("7. restart container", before, rss(CONTAINER), "", resources(CONTAINER))
        log = sh("docker", "logs", CONTAINER).stdout + \
              sh("docker", "logs", CONTAINER).stderr
        reclaim_lines = [line for line in log.splitlines() if "[reclaim]" in line]
        if reclaim_lines:
            print("\n  container [reclaim] log:")
            for line in reclaim_lines[-8:]:
                print(f"    {line.strip()[:150]}")
    finally:
        emulator.stop()

    print("\n| Attempt | Before | After | % of cap | CPU | Result |")
    print("| --- | ---: | ---: | ---: | ---: | --- |")
    for name, before, after, verdict, note, pct, cpu in RESULTS:
        suffix = f" ({note})" if note else ""
        print(f"| {name} | {before:.1f} MiB | {after:.1f} MiB | "
              f"{pct:.1f}% | {cpu:.1f}% | {verdict}{suffix} |")

    baseline_rss = RESULTS[-1][2] if RESULTS else float("nan")
    print(f"\nPeak RSS {PEAK['rss']:.1f} MiB, peak CPU {PEAK['cpu']:.1f}%, "
          f"final {baseline_rss:.1f} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
