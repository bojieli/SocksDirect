#!/usr/bin/env bash
# run-demo.sh — boot a tiny nginx under LD_PRELOAD for demo purposes.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

LIBSD="${LIBSD:-$REPO/build/libsd.so}"
PORT="${PORT:-58080}"
WORKDIR="${WORKDIR:-$HERE/.run}"
mkdir -p "$WORKDIR/www"
echo "<html><body>SocksDirect nginx demo</body></html>" > "$WORKDIR/www/index.html"

cat > "$WORKDIR/nginx.conf" <<EOF
worker_processes 1;
daemon off;
error_log $WORKDIR/error.log warn;
pid $WORKDIR/nginx.pid;
events { worker_connections 256; }
http {
    access_log off;
    server {
        listen $PORT;
        root $WORKDIR/www;
    }
}
EOF

if [[ -f "$LIBSD" ]]; then
    echo "starting nginx with LD_PRELOAD=$LIBSD on port $PORT"
    exec env LD_PRELOAD="$LIBSD" nginx -c "$WORKDIR/nginx.conf" -p "$WORKDIR"
else
    echo "libsd not built; running nginx without preload (port $PORT)"
    exec nginx -c "$WORKDIR/nginx.conf" -p "$WORKDIR"
fi
