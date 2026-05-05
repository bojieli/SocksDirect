#!/usr/bin/env bash
# tab1-latency-breakdown — per-segment cycle counts on the hot path.
# Paper Table 1; the original paper hand-built this. We instrument the
# bench_queue_v3 latency mode and report rdtsc segments.
#
# A full version requires libsd-side rdtsc markers (Phase 6 follow-up);
# this script publishes the queue-only segment so the harness has data
# to render and the report doesn't ship blanks.

. "$(dirname "$0")/../_lib.sh"
require_repro_env

BIN="${REPRO_REPO}/build/bench/microbench/bench_queue_v3"
[[ -x "$BIN" ]] || skip_figure "bench_queue_v3 not built"

RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
"$BIN" --mode=latency --count=200000 \
    | python3 -c "
import json, sys
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    o = json.loads(line)
    o['bench']   = 'tab1_latency_breakdown'
    o['submode'] = 'segment=queue_round_trip'
    print(json.dumps(o))
" > "$RAW"

emit_csv
maybe_plot bar "latency breakdown (partial; queue segment only)"
echo "Tier ${REPRO_TIER}: tab1-latency-breakdown complete (partial; full instrumentation TODO)."
