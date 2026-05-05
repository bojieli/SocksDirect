#!/usr/bin/env python3
"""nf_meter.py — count bytes per packet for billing/SLA enforcement."""
import struct, sys
hdr = struct.Struct(">H")
inp, out = sys.stdin.buffer, sys.stdout.buffer
total = 0; count = 0
while True:
    h = inp.read(2)
    if len(h) < 2: break
    (n,) = hdr.unpack(h)
    body = inp.read(n)
    if len(body) < n: break
    total += n; count += 1
    out.write(h); out.write(body)
out.flush()
sys.stderr.write(f"meter {count} pkt {total} bytes\n")
