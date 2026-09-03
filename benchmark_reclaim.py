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


def rss(container):
    out = sh("docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", container).stdout
    value = out.split("/")[0].strip()
    if not value:
        return float("nan")
    if "GiB" in value:
        return float(value.replace("GiB", "")) * 1024
    if "KiB" in value:
        return float(value.replace("KiB", "")) / 1024
    return float(value.replace("MiB", ""))


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


def record(name, before, after, note=""):
    delta = after - before
    if delta < -1:
        verdict = f"RECLAIMED {-delta:.1f} MiB"
    elif delta > 1:
        verdict = f"+{delta:.1f} MiB"
    else:
        verdict = "no change"
    RESULTS.append((name, before, after, verdict, note))
    print(f"  {name:42} {before:7.1f} -> {after:7.1f} MiB   {verdict} {note}")


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
        record("1. mutation delete, keySet all (30k)", before, rss(CONTAINER), note)

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
        record("2. DML DELETE FROM t WHERE true (10k)", before, rss(CONTAINER), note)

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
        record("3. DROP TABLE x10", before, rss(CONTAINER), note)

        # --- 4. DROP DATABASE ---------------------------------------------
        before = rss(CONTAINER)
        emulator.delete(f"projects/{PROJECT}/instances/{INSTANCE}/databases/benchthree")
        time.sleep(5)
        record("4. DROP DATABASE", before, rss(CONTAINER))

        # --- 6. DROP INDEX x142 (before deleting the instance) -------------
        # One column per index: repeating (table, column) pairs is rejected.
        statements = ["CREATE TABLE W (id INT64 NOT NULL, "
                      + ", ".join(f"c{i} STRING(MAX)" for i in range(142)) + ") PRIMARY KEY (id)"]
        statements += [f"CREATE INDEX ix{i} ON W(c{i})" for i in range(142)]
        emulator.create_database("benchidx", statements)
        emulator.ddl("benchidx", [
            f"ALTER DATABASE benchidx SET OPTIONS (version_retention_period = '{args.retention}')"])
        session4 = emulator.session("benchidx")
        before = rss(CONTAINER)
        emulator.ddl("benchidx", [f"DROP INDEX ix{i}" for i in range(142)])
        time.sleep(4)
        note = ""
        if args.reclaim:
            note = "purged=%s/%s" % emulator.reclaim(session4)
        record("6. DROP INDEX x142", before, rss(CONTAINER), note)

        # --- 5. DELETE INSTANCE -------------------------------------------
        before = rss(CONTAINER)
        emulator.delete(f"projects/{PROJECT}/instances/{INSTANCE}")
        time.sleep(6)
        record("5. DELETE INSTANCE", before, rss(CONTAINER))

        # --- 7. restart container -----------------------------------------
        before = rss(CONTAINER)
        emulator.stop()
        emulator.start()
        record("7. restart container", before, rss(CONTAINER))
    finally:
        emulator.stop()

    print("\n| Attempt | Before | After | Result |")
    print("| --- | ---: | ---: | --- |")
    for name, before, after, verdict, note in RESULTS:
        suffix = f" ({note})" if note else ""
        print(f"| {name} | {before:.1f} MiB | {after:.1f} MiB | {verdict}{suffix} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
