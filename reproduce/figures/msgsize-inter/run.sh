#!/usr/bin/env bash
# msgsize-inter — inter-host throughput vs message size (paper Fig 3-right).
#
# Tier 3 (real RDMA across two hosts). Skips cleanly otherwise — and
# specifically refuses to publish numbers when running over SoftRoCE,
# since those are an order of magnitude off the paper.

. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_two_hosts
require_libsd 3

# Read the inventory for the peer host's IP and SSH target.
PEER_IP=$(python3 -c "
import sys
sys.path.insert(0, '${REPRO_REPO}/reproduce')
import yaml
inv = yaml.safe_load(open('${REPRO_INVENTORY}'))
for h, info in (inv.get('hosts') or {}).items():
    if h != '$(hostname -s)':
        print(info.get('rdma_ip',''))
        break
" 2>/dev/null)
PEER_SSH=$(python3 -c "
import sys, yaml
inv = yaml.safe_load(open('${REPRO_INVENTORY}'))
for h, info in (inv.get('hosts') or {}).items():
    if h != '$(hostname -s)':
        print(info.get('ssh',''))
        break
" 2>/dev/null)
[[ -n "$PEER_IP" && -n "$PEER_SSH" ]] || skip_figure "inventory.yml has no usable peer host"

LIBSD="${REPRO_REPO}/build/libsd.so"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
: > "$RAW"

S="${REPRO_REPO}/build/pot_eval_inter_host_tput_s"
C="${REPRO_REPO}/build/pot_eval_inter_host_tput_c"
[[ -x "$S" && -x "$C" ]] || skip_figure "pot_eval_inter_host_tput_{s,c} not built"

SIZES="${SIZES:-8 64 256 1024 4096 16384 65536 262144 1048576}"
for size in $SIZES; do
    ssh -n "$PEER_SSH" \
        "cd ${REPRO_REPO} && LD_PRELOAD=${LIBSD} ./build/pot_eval_inter_host_tput_s $size" \
        >/dev/null 2>&1 &
    SPID=$!
    sleep 1
    LD_PRELOAD="$LIBSD" "$C" "$PEER_IP" "$size" 1 2>/dev/null \
        | python3 -c "
import json, sys
for line in sys.stdin:
    if line.startswith('throughput_mps='):
        v = float(line.split('=',1)[1])
        print(json.dumps({'bench':'msgsize_inter','submode':f'msgsize=$size',
                          'mode':'throughput','msg_count':0,'elapsed_ns':0,
                          'throughput_mps':v,'p50_ns':None,'p99_ns':None,'p999_ns':None,
                          'transport':'rdma'}))
" >> "$RAW" || true
    wait $SPID 2>/dev/null || true
done

emit_csv
maybe_plot line "inter-host throughput vs msg size"
echo "Tier ${REPRO_TIER}: msgsize-inter complete."
