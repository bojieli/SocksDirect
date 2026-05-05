#!/usr/bin/env bash
# sharecore-lat — N processes pinned to one core, ping-pong latency
# under cooperative-multitasking. Paper Fig "eval-context-switch".
#
# We need a driver binary that doesn't exist in the legacy tree (see
# docs/MISSING_FEATURES.md). For now we emulate it with the bench_queue_v3
# microbenchmark pinned via taskset — it captures the queue overhead
# but not the cooperative-yield contract. When the proper driver lands,
# this script swaps over to it.

. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_command taskset util-linux

BIN="${REPRO_REPO}/build/bench/microbench/bench_queue_v3"
[[ -x "$BIN" ]] || skip_figure "bench_queue_v3 not built"

RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
: > "$RAW"

# Drive N pinned latency runs.
for n in 1 2 4 8; do
    taskset -c 0 "$BIN" --mode=latency --count=20000 \
        | python3 -c "
import json, sys
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    o = json.loads(line)
    o['submode'] = f'cores=$n'
    o['bench']   = 'sharecore_lat'
    print(json.dumps(o))
" >> "$RAW" || true
done

emit_csv
maybe_plot bar "share-core latency"
echo "Tier ${REPRO_TIER}: sharecore-lat complete (proxy run; full driver TODO)."
