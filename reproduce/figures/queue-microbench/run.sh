#!/usr/bin/env bash
# queue-microbench — exercise the standalone lockless-queue benchmark.
#
# This figure is the simplest reproducible artifact in the harness.
# It runs entirely on one host (no SSH, no RDMA) and produces a CSV
# with throughput and latency numbers for the underlying SHM queue.
# Useful as a sanity check that ./repro and the build are wired up.
#
# Inputs (all from the harness env):
#   REPRO_REPO         absolute path to the SocksDirect tree
#   REPRO_RESULTS_DIR  directory to drop result.csv into
#   REPRO_TIER         resolved reproduction tier (1, 2, or 3)

set -euo pipefail
: "${REPRO_REPO:?must be set by ./repro}"
: "${REPRO_RESULTS_DIR:?must be set by ./repro}"

BENCH="${REPRO_REPO}/build/bench/microbench/bench_queue_v3"
if [[ ! -x "${BENCH}" ]]; then
    echo "error: ${BENCH} not built. Run:" >&2
    echo "  cmake -S ${REPRO_REPO} -B ${REPRO_REPO}/build -DSOCKSDIRECT_WITH_RDMA=OFF" >&2
    echo "  cmake --build ${REPRO_REPO}/build -j" >&2
    exit 3
fi

mkdir -p "${REPRO_RESULTS_DIR}"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
CSV="${REPRO_RESULTS_DIR}/result.csv"
: > "${RAW}"

# Multiple iterations to capture noise.
for iter in 1 2 3; do
    "${BENCH}" --mode=throughput --count=2000000 >> "${RAW}"
    "${BENCH}" --mode=latency --count=50000     >> "${RAW}"
done

# Convert to a flat CSV the plot template can consume.
python3 - <<PY > "${CSV}"
import json, sys
rows = [("iter","mode","throughput_mps","p50_ns","p99_ns","p999_ns")]
with open("${RAW}") as fh:
    by_mode = {"throughput": [], "latency": []}
    for line in fh:
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        by_mode[o["mode"]].append(o)

for i, t in enumerate(by_mode.get("throughput", []), start=1):
    rows.append((str(i), "throughput", str(t["throughput_mps"]), "", "", ""))
for i, t in enumerate(by_mode.get("latency", []), start=1):
    rows.append((str(i), "latency", "", str(t["p50_ns"]), str(t["p99_ns"]), str(t["p999_ns"])))

import csv, io
out = io.StringIO()
csv.writer(out).writerows(rows)
sys.stdout.write(out.getvalue())
PY

echo "Wrote ${CSV}"
echo "Tier ${REPRO_TIER}: queue-microbench complete."
