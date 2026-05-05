#!/usr/bin/env python3
"""sink.py — drain the pipeline, count packets, print a one-liner."""
import struct, sys, time
hdr = struct.Struct(">H")
inp = sys.stdin.buffer
count = 0
t0 = time.monotonic()
while True:
    h = inp.read(2)
    if len(h) < 2: break
    (n,) = hdr.unpack(h)
    body = inp.read(n)
    if len(body) < n: break
    count += 1
elapsed = time.monotonic() - t0
sys.stderr.write(f"sink got {count} pkt in {elapsed:.3f}s ({count/max(elapsed,1e-9):.0f} pps)\n")
