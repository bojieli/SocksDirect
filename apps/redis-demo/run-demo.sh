#!/usr/bin/env bash
# run-demo.sh — boot redis-server under preload.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LIBSD="${LIBSD:-$REPO/build/libsd.so}"
PORT="${PORT:-56379}"
WORKDIR="${WORKDIR:-$HERE/.run}"
mkdir -p "$WORKDIR"
cat > "$WORKDIR/redis.conf" <<EOF
port $PORT
bind 127.0.0.1
save ""
appendonly no
dir $WORKDIR
logfile $WORKDIR/redis.log
EOF
if command -v redis-server >/dev/null 2>&1; then
    if [[ -f "$LIBSD" ]]; then
        echo "starting redis-server with preload on port $PORT"
        exec env LD_PRELOAD="$LIBSD" redis-server "$WORKDIR/redis.conf"
    fi
    echo "libsd not built; running redis without preload (port $PORT)"
    exec redis-server "$WORKDIR/redis.conf"
else
    echo "redis-server not installed (apt install redis-server)" >&2
    exit 3
fi
