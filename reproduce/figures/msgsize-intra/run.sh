#!/usr/bin/env bash
# msgsize-intra — intra-host throughput vs message size (paper Fig 3-left).
#
# Drives the legacy pot/eval_intra_host_tput_* binaries under preload
# and aggregates the JSONL output. Falls back to a graceful skip when
# libsd or the bench binaries aren't built.

. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_libsd 2

LIBSD="${REPRO_REPO}/build/libsd.so"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
: > "$RAW"

# The legacy pot binaries are gated on SOCKSDIRECT_WITH_RDMA + WITH_HERD;
# if they're not present, skip cleanly.
S="${REPRO_REPO}/build/pot_eval_intra_host_tput_s"
C="${REPRO_REPO}/build/pot_eval_intra_host_tput_c"
[[ -x "$S" && -x "$C" ]] || skip_figure "pot_eval_intra_host_tput_{s,c} not built"

SIZES="${SIZES:-8 64 256 1024 4096 16384 65536 262144 1048576}"
for size in $SIZES; do
    LD_PRELOAD="$LIBSD" "$S" "$size" >/dev/null 2>&1 &
    SPID=$!
    sleep 0.5
    LD_PRELOAD="$LIBSD" "$C" 127.0.0.1 "$size" 1 \
        | python3 -c "
import json, sys
for line in sys.stdin:
    line = line.strip()
    if line.startswith('throughput_mps='):
        v = float(line.split('=',1)[1])
        print(json.dumps({'bench':'msgsize_intra','submode':f'msgsize=$size',
                          'mode':'throughput','msg_count':0,'elapsed_ns':0,
                          'throughput_mps':v,'p50_ns':None,'p99_ns':None,'p999_ns':None}))
" >> "$RAW" || true
    kill $SPID 2>/dev/null || true
    wait $SPID 2>/dev/null || true
done

emit_csv
maybe_plot line "intra-host throughput vs msg size"
echo "Tier ${REPRO_TIER}: msgsize-intra complete."
