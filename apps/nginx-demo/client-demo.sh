#!/usr/bin/env bash
# client-demo.sh — fire 1000 requests at the demo nginx; print median.
set -euo pipefail
PORT="${PORT:-58080}"
N="${N:-1000}"
TS=$(mktemp)
for i in $(seq 1 "$N"); do
    /usr/bin/time -f "%e" -o "$TS" -a curl -s "http://127.0.0.1:$PORT/" > /dev/null
done
python3 - "$TS" <<'PY'
import sys, statistics
xs = [float(x) for x in open(sys.argv[1])]
print(f"{len(xs)} requests; median {statistics.median(xs)*1000:.2f} ms")
PY
rm -f "$TS"
