#!/usr/bin/env bash
# client-demo.sh — fire redis-benchmark at the demo server.
set -euo pipefail
PORT="${PORT:-56379}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LIBSD="${LIBSD:-$REPO/build/libsd.so}"
N="${N:-10000}"
if ! command -v redis-benchmark >/dev/null 2>&1; then
    echo "redis-benchmark not installed (apt install redis-tools)" >&2
    exit 3
fi
ARGS=(-h 127.0.0.1 -p "$PORT" -n "$N" -t set,get -q)
if [[ -f "$LIBSD" ]]; then
    LD_PRELOAD="$LIBSD" redis-benchmark "${ARGS[@]}"
else
    redis-benchmark "${ARGS[@]}"
fi
