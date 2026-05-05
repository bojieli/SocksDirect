#!/usr/bin/env python3
"""perf_regression.py — compare benchmark JSON output against a baseline.

Reads benchmark JSON (one JSON object per line, as emitted by
bench/microbench/bench_*) from --current and from --baseline. Pairs them
up by (bench, mode) and reports PASS/REGRESSED/IMPROVED for each pair.

Exit codes:
  0  all pairs pass within --threshold (default 10%)
  1  at least one pair regressed
  2  invalid input

Used by the CI perf-regression gate. Run on a self-hosted runner with
pinned CPUs; the public GitHub-hosted runners are too noisy for stable
microbenchmark numbers.

Usage:
  perf_regression.py --baseline baseline.json --current current.json [--threshold 0.10]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Iterable


def load_results(path: Path) -> dict[tuple[str, str], dict]:
    out: dict[tuple[str, str], dict] = {}
    with path.open() as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            key = (obj["bench"], obj["mode"])
            out[key] = obj
    return out


def compare(baseline: dict, current: dict, mode: str) -> tuple[str, str]:
    """Return (verdict, detail). verdict is one of PASS|REGRESSED|IMPROVED|MISSING."""
    if mode == "throughput":
        b = baseline.get("throughput_mps")
        c = current.get("throughput_mps")
        if b is None or c is None:
            return "MISSING", "throughput_mps not present"
        delta = (c - b) / b
        # Higher is better.
        if delta < -threshold_global:
            return "REGRESSED", f"throughput {b:.1f} -> {c:.1f} mps ({delta:+.1%})"
        if delta > threshold_global:
            return "IMPROVED", f"throughput {b:.1f} -> {c:.1f} mps ({delta:+.1%})"
        return "PASS", f"throughput {b:.1f} -> {c:.1f} mps ({delta:+.1%})"
    elif mode == "latency":
        b = baseline.get("p50_ns")
        c = current.get("p50_ns")
        if b is None or c is None:
            return "MISSING", "p50_ns not present"
        delta = (c - b) / b
        # Lower is better; flip the sign for the verdict.
        if delta > threshold_global:
            return "REGRESSED", f"p50 latency {b} -> {c} ns ({delta:+.1%})"
        if delta < -threshold_global:
            return "IMPROVED", f"p50 latency {b} -> {c} ns ({delta:+.1%})"
        return "PASS", f"p50 latency {b} -> {c} ns ({delta:+.1%})"
    else:
        return "MISSING", f"unknown mode {mode}"


threshold_global = 0.10  # set in main()


def main(argv: list[str]) -> int:
    global threshold_global
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--current", type=Path, required=True)
    p.add_argument("--threshold", type=float, default=0.10,
                   help="fractional regression tolerance (default 0.10 = 10%%)")
    args = p.parse_args(argv)
    threshold_global = args.threshold

    baseline = load_results(args.baseline)
    current = load_results(args.current)

    keys = sorted(set(baseline) | set(current))
    any_regressed = False
    for bench, mode in keys:
        if (bench, mode) not in baseline:
            print(f"[NEW]       {bench}/{mode}: {current[(bench, mode)]}")
            continue
        if (bench, mode) not in current:
            print(f"[MISSING]   {bench}/{mode}: present in baseline only")
            any_regressed = True
            continue
        verdict, detail = compare(baseline[(bench, mode)], current[(bench, mode)], mode)
        prefix = {
            "PASS":      "[PASS]     ",
            "REGRESSED": "[REGRESS]  ",
            "IMPROVED":  "[IMPROVED] ",
            "MISSING":   "[MISSING]  ",
        }[verdict]
        print(f"{prefix}{bench}/{mode}: {detail}")
        if verdict == "REGRESSED":
            any_regressed = True

    return 1 if any_regressed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
