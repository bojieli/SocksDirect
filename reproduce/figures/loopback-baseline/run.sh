#!/usr/bin/env bash
# loopback-baseline — Linux TCP loopback ping-pong.
#
# This is the *reference* number that intra-host figures need to
# compare against. Producing it via the harness — and as a Tier-1
# figure that needs nothing more than a Linux box — means a reader
# can reproduce the comparison context even without RDMA, hugepages,
# or the kernel module.
#
# Inputs:
#   REPRO_REPO         absolute path to the SocksDirect tree
#   REPRO_RESULTS_DIR  directory to drop result.csv into
#   REPRO_TIER         resolved reproduction tier (1, 2, or 3)

set -euo pipefail
: "${REPRO_REPO:?must be set by ./repro}"
: "${REPRO_RESULTS_DIR:?must be set by ./repro}"

BIN="${REPRO_REPO}/build/bench/baselines/linux_tcp_pingpong"
if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Run:" >&2
    echo "  cmake -S ${REPRO_REPO} -B ${REPRO_REPO}/build -DSOCKSDIRECT_WITH_RDMA=OFF" >&2
    echo "  cmake --build ${REPRO_REPO}/build -j" >&2
    exit 3
fi

mkdir -p "${REPRO_RESULTS_DIR}"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
CSV="${REPRO_RESULTS_DIR}/result.csv"
: > "${RAW}"

# 3 iterations for noise estimation.
for iter in 1 2 3; do
    "${BIN}" --iters=20000 >> "${RAW}"
done

python3 - <<PY > "${CSV}"
import json, csv, io, sys
rows = [("iter","bench","submode","mode","msg_size","throughput_mps","p50_ns","p99_ns","p999_ns")]
counts = {}
with open("${RAW}") as fh:
    for line in fh:
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        sub = o.get("submode", "")
        msgsize = sub.split("=", 1)[1] if "=" in sub else ""
        key = (o["bench"], sub, o["mode"])
        counts[key] = counts.get(key, 0) + 1
        rows.append((
            str(counts[key]),
            o["bench"],
            sub,
            o["mode"],
            msgsize,
            "" if o.get("throughput_mps") is None else str(o["throughput_mps"]),
            "" if o.get("p50_ns") is None else str(o["p50_ns"]),
            "" if o.get("p99_ns") is None else str(o["p99_ns"]),
            "" if o.get("p999_ns") is None else str(o["p999_ns"]),
        ))

out = io.StringIO()
csv.writer(out).writerows(rows)
sys.stdout.write(out.getvalue())
PY

echo "Wrote ${CSV}"
echo "Tier ${REPRO_TIER}: loopback-baseline complete."
