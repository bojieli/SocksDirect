#!/usr/bin/env python3
"""source.py — emit a stream of length-prefixed 64-byte packets.

Each packet is `len:uint16-be` || `payload[len]`. We use a fixed
64-byte payload so the downstream NFs don't have to size buffers
dynamically. Counts up; emits `--count` packets total then EOF.
"""
import argparse, os, struct, sys

p = argparse.ArgumentParser()
p.add_argument("--count", type=int, default=10000)
p.add_argument("--size",  type=int, default=64)
args = p.parse_args()

out = sys.stdout.buffer
hdr = struct.Struct(">H")
pkt = bytes(args.size)
for _ in range(args.count):
    out.write(hdr.pack(args.size))
    out.write(pkt)
out.flush()
