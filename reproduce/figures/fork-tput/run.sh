#!/usr/bin/env bash
# fork-tput — connection-setup throughput via fork().
. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_libsd 2

LIBSD="${REPRO_REPO}/build/libsd.so"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
: > "$RAW"

# pot_eval_fork_* binaries: server forks N children that each do one
# accept/recv/send/close.
S="${REPRO_REPO}/build/pot_eval_fork_s"
C="${REPRO_REPO}/build/pot_eval_fork_c"
[[ -x "$S" && -x "$C" ]] || skip_figure "pot_eval_fork_{s,c} not built"

CONNS="${CONNS:-1000}"
LD_PRELOAD="$LIBSD" "$S" >/dev/null 2>&1 &
SPID=$!
sleep 0.5
LD_PRELOAD="$LIBSD" "$C" 127.0.0.1 "$CONNS" 2>/dev/null \
    | python3 -c "
import json, sys
for line in sys.stdin:
    if line.startswith('connections_per_sec='):
        v = float(line.split('=',1)[1])
        print(json.dumps({'bench':'fork_tput','submode':f'conns=$CONNS',
                          'mode':'throughput','msg_count':$CONNS,'elapsed_ns':0,
                          'throughput_mps':v,'p50_ns':None,'p99_ns':None,'p999_ns':None}))
" >> "$RAW" || true
kill $SPID 2>/dev/null || true
wait $SPID 2>/dev/null || true

emit_csv
maybe_plot bar "fork+accept connections per second"
echo "Tier ${REPRO_TIER}: fork-tput complete."
