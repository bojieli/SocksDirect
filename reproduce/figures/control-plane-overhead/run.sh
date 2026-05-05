#!/usr/bin/env bash
# control-plane-overhead — measure the cost of the control-plane bits
# (config lookup, fd remap, metrics inc) that sit on the socket setup
# path. Like queue-microbench, this runs on one host with no RDMA.
#
# Why a separate figure: socket_lib calls these on every accept/connect
# /close. The intra-host throughput figures will charge SocksDirect with
# overhead that actually belongs to the control plane unless we
# attribute it explicitly.
#
# Inputs (all from the harness env):
#   REPRO_REPO         absolute path to the SocksDirect tree
#   REPRO_RESULTS_DIR  directory to drop result.csv into
#   REPRO_TIER         resolved reproduction tier (1, 2, or 3)

set -euo pipefail
: "${REPRO_REPO:?must be set by ./repro}"
: "${REPRO_RESULTS_DIR:?must be set by ./repro}"

BENCH_DIR="${REPRO_REPO}/build/bench/microbench"
for b in bench_fd_remap bench_metrics bench_config; do
    if [[ ! -x "${BENCH_DIR}/${b}" ]]; then
        echo "error: ${BENCH_DIR}/${b} not built. Run:" >&2
        echo "  cmake -S ${REPRO_REPO} -B ${REPRO_REPO}/build -DSOCKSDIRECT_WITH_RDMA=OFF" >&2
        echo "  cmake --build ${REPRO_REPO}/build -j" >&2
        exit 3
    fi
done

mkdir -p "${REPRO_RESULTS_DIR}"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
CSV="${REPRO_RESULTS_DIR}/result.csv"
: > "${RAW}"

# Three iterations, each recording all three benches.
for iter in 1 2 3; do
    "${BENCH_DIR}/bench_fd_remap"  --mode=throughput --count=500000 --threads=2 >> "${RAW}"
    "${BENCH_DIR}/bench_fd_remap"  --mode=latency    --count=20000             >> "${RAW}"
    "${BENCH_DIR}/bench_metrics"   --mode=throughput --count=2000000 --threads=2 >> "${RAW}"
    "${BENCH_DIR}/bench_config"    --count=500000                              >> "${RAW}"
done

# Flatten to a CSV the plot template can read.
python3 - <<PY > "${CSV}"
import json, sys, csv, io
rows = [("iter", "bench", "submode", "mode", "throughput_mps", "p50_ns")]
counts = {}
with open("${RAW}") as fh:
    for line in fh:
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        bench = o["bench"]
        sub   = o.get("submode", "")
        key   = (bench, sub, o["mode"])
        counts[key] = counts.get(key, 0) + 1
        rows.append((
            str(counts[key]),
            bench,
            sub,
            o["mode"],
            "" if o.get("throughput_mps") is None else str(o["throughput_mps"]),
            "" if o.get("p50_ns") is None else str(o["p50_ns"]),
        ))

out = io.StringIO()
csv.writer(out).writerows(rows)
sys.stdout.write(out.getvalue())
PY

echo "Wrote ${CSV}"
echo "Tier ${REPRO_TIER}: control-plane-overhead complete."
