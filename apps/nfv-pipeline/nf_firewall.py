#!/usr/bin/env python3
"""nf_firewall.py — drop packets matching a counter-controlled rule.
For the demo we accept everything; the NF still pays the per-packet
parse + counter-update cost.
"""
import struct, sys
hdr = struct.Struct(">H")
inp, out = sys.stdin.buffer, sys.stdout.buffer
counted = 0
while True:
    h = inp.read(2)
    if len(h) < 2: break
    (n,) = hdr.unpack(h)
    body = inp.read(n)
    if len(body) < n: break
    counted += 1
    out.write(h); out.write(body)
out.flush()
sys.stderr.write(f"firewall passed {counted} packets\n")
