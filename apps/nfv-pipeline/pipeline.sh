#!/usr/bin/env bash
# pipeline.sh — driver for the NFV pipeline app.
#
# Spawns: pcap_source | nf_firewall | nf_meter | nf_nat | sink
# Each NF is a small Python program that reads packets from stdin
# (length-prefixed bytes), updates a counter, and writes them to stdout.
# When run under `LD_PRELOAD=libsd.so`, the pipes become libsd
# accelerated sockets (the lib intercepts pipe creation when set up via
# socketpair).
#
# Usage:
#   ./pipeline.sh --packets 100000

set -euo pipefail
PACKETS=100000
while [[ $# -gt 0 ]]; do
    case "$1" in
        --packets) PACKETS="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
"$HERE/source.py" --count "$PACKETS" \
    | "$HERE/nf_firewall.py" \
    | "$HERE/nf_meter.py" \
    | "$HERE/nf_nat.py" \
    | "$HERE/sink.py"
