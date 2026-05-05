#!/usr/bin/env bash
# corenum-intra — intra-host throughput as core count scales.
. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_libsd 2

LIBSD="${REPRO_REPO}/build/libsd.so"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
: > "$RAW"

S="${REPRO_REPO}/build/pot_eval_intra_host_tput_s"
C="${REPRO_REPO}/build/pot_eval_intra_host_tput_c"
[[ -x "$S" && -x "$C" ]] || skip_figure "pot_eval_intra_host_tput_{s,c} not built"

MAX_CORES="${MAX_CORES:-$(nproc)}"
SIZE="${SIZE:-64}"
for n in 1 2 4 8 12 16; do
    [[ "$n" -gt "$MAX_CORES" ]] && break
    LD_PRELOAD="$LIBSD" "$S" "$SIZE" >/dev/null 2>&1 &
    SPID=$!
    sleep 0.5
    LD_PRELOAD="$LIBSD" "$C" 127.0.0.1 "$SIZE" "$n" 2>/dev/null \
        | python3 -c "
import json, sys
for line in sys.stdin:
    if line.startswith('throughput_mps='):
        v = float(line.split('=',1)[1])
        print(json.dumps({'bench':'corenum_intra','submode':f'cores=$n',
                          'mode':'throughput','msg_count':0,'elapsed_ns':0,
                          'throughput_mps':v,'p50_ns':None,'p99_ns':None,'p999_ns':None}))
" >> "$RAW" || true
    kill $SPID 2>/dev/null || true
    wait $SPID 2>/dev/null || true
done

emit_csv
maybe_plot bar "intra-host throughput vs core count"
echo "Tier ${REPRO_TIER}: corenum-intra complete."
