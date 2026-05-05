#!/usr/bin/env bash
# nfv — pcap-driven NFV pipeline of small NFs piped together via libsd.
. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_libsd 2

PIPE="${REPRO_REPO}/apps/nfv-pipeline/pipeline.sh"
[[ -x "$PIPE" ]] || skip_figure "apps/nfv-pipeline/pipeline.sh missing"

LIBSD="${REPRO_REPO}/build/libsd.so"
RAW="${REPRO_RESULTS_DIR}/raw.jsonl"
N=${N:-100000}
START=$(date +%s.%N)
LD_PRELOAD="$LIBSD" "$PIPE" --packets "$N" >/dev/null
END=$(date +%s.%N)
elapsed=$(python3 -c "print($END-$START)")
rate=$(python3 -c "print($N/$elapsed)")
python3 -c "
import json
print(json.dumps({'bench':'nfv','submode':'pipeline','mode':'throughput',
                  'msg_count':$N,'elapsed_ns':int($elapsed*1e9),
                  'throughput_mps':$rate,'p50_ns':None,'p99_ns':None,'p999_ns':None}))
" > "$RAW"
emit_csv
echo "Tier ${REPRO_TIER}: nfv pipeline ${rate} pps."
