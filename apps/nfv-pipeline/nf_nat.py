#!/usr/bin/env python3
"""nf_nat.py — swap the first byte of each payload (toy NAT)."""
import struct, sys
hdr = struct.Struct(">H")
inp, out = sys.stdin.buffer, sys.stdout.buffer
while True:
    h = inp.read(2)
    if len(h) < 2: break
    (n,) = hdr.unpack(h)
    body = inp.read(n)
    if len(body) < n: break
    if body:
        body = bytes([body[0] ^ 0xff]) + body[1:]
    out.write(h); out.write(body)
out.flush()
