#!/usr/bin/env bash
# nginx — request rate served by nginx under preload.
. "$(dirname "$0")/../_lib.sh"
require_repro_env
require_libsd 2
require_command nginx nginx
require_command curl curl

LIBSD="${REPRO_REPO}/build/libsd.so"
APP_DIR="${REPRO_REPO}/apps/nginx-demo"
[[ -d "$APP_DIR" ]] || skip_figure "apps/nginx-demo missing"

NGINX_CONF="$REPRO_RESULTS_DIR/nginx.conf"
DOCROOT="$REPRO_RESULTS_DIR/www"
mkdir -p "$DOCROOT"
echo "ok" > "$DOCROOT/index.html"

PORT="${PORT:-58080}"
cat > "$NGINX_CONF" <<EOF
worker_processes 1;
daemon off;
error_log $REPRO_RESULTS_DIR/nginx.err warn;
pid $REPRO_RESULTS_DIR/nginx.pid;
events { worker_connections 256; }
http {
    access_log off;
    server {
        listen $PORT;
        root $DOCROOT;
    }
}
EOF

LD_PRELOAD="$LIBSD" nginx -c "$NGINX_CONF" -p "$REPRO_RESULTS_DIR" >/dev/null 2>&1 &
NPID=$!
sleep 0.5
RAW="$REPRO_RESULTS_DIR/raw.jsonl"
: > "$RAW"
N=${N:-2000}
START=$(date +%s.%N)
for i in $(seq 1 "$N"); do
    curl -s "http://127.0.0.1:$PORT/" > /dev/null || true
done
END=$(date +%s.%N)
elapsed=$(python3 -c "print($END-$START)")
rps=$(python3 -c "print($N/$elapsed)")
python3 -c "
import json
print(json.dumps({'bench':'nginx','submode':'GET_/','mode':'throughput',
                  'msg_count':$N,'elapsed_ns':int($elapsed*1e9),
                  'throughput_mps':$rps,'p50_ns':None,'p99_ns':None,'p999_ns':None}))
" >> "$RAW"

kill -TERM $NPID 2>/dev/null || true
wait $NPID 2>/dev/null || true
emit_csv
echo "Tier ${REPRO_TIER}: nginx complete (~${rps} req/s)."
