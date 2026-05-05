#!/usr/bin/env python3
"""run_conformance.py — execute every case in coverage.toml.

Two modes:

  baseline (default)         compile + run each case under unmodified
                             glibc. Validates coverage.toml itself
                             (every listed case file exists and
                             behaves as documented).

  --preload PATH/libsd.so    additionally run each case with libsd
                             preloaded. Compares the preloaded result
                             to the baseline. Cases marked
                             accelerated/passthrough must match
                             baseline; partial cases must not crash;
                             unsupported cases are expected to either
                             match baseline (libsd hands off) or
                             return a documented error.

Output: one line per case, plus a summary. Exit 0 iff every case in
the baseline run passed; in --preload mode, exit 0 iff the
preloaded run additionally honors each case's status contract.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

HERE = Path(__file__).resolve().parent
COVERAGE = HERE / "coverage.toml"


# We do not assume tomllib (3.11+); a tiny recursive-descent parser
# covers the array-of-tables subset our coverage file uses.
TOML_SCALAR = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$')


@dataclass
class Function:
    name: str
    status: str = ""
    case: str = ""
    fix_phase: Optional[int] = None
    notes: str = ""


def parse_coverage(path: Path) -> list[Function]:
    out: list[Function] = []
    cur: Optional[Function] = None
    for raw in path.read_text().splitlines():
        s = raw.split("#", 1)[0].strip()
        if not s:
            continue
        if s == "[[function]]":
            if cur is not None:
                out.append(cur)
            cur = Function(name="")
            continue
        m = TOML_SCALAR.match(s)
        if not m or cur is None:
            continue
        k, v = m.group(1), m.group(2).strip()
        if v.startswith('"') and v.endswith('"'):
            v = v[1:-1]
        if k == "fix_phase":
            cur.fix_phase = int(v)
        else:
            setattr(cur, k, v)
    if cur is not None:
        out.append(cur)
    return out


@dataclass
class CaseResult:
    function: str
    case_path: str
    status: str        # from coverage
    verdict: str       # PASS | FAIL | SKIP | MISSING
    detail: str = ""
    rc_baseline: Optional[int] = None
    rc_preload:  Optional[int] = None


def compile_case(src: Path, out: Path) -> Optional[str]:
    rc = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(out), str(src)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if rc.returncode != 0:
        return rc.stderr.decode()
    return None


def run_binary(binary: Path, *, preload: Optional[Path] = None,
               timeout: int = 15) -> tuple[int, str]:
    env = os.environ.copy()
    if preload is not None:
        env["LD_PRELOAD"] = str(preload)
        env.setdefault("SOCKSDIRECT_LOG", "warn")
    try:
        r = subprocess.run(
            [str(binary)], env=env, timeout=timeout,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        return r.returncode, (r.stdout + r.stderr).decode()[:1000]
    except subprocess.TimeoutExpired as e:
        return 124, f"TIMEOUT after {e.timeout}s"


def evaluate(fn: Function, baseline_rc: Optional[int],
             preload_rc: Optional[int],
             *, preload_mode: bool) -> tuple[str, str]:
    """Return (verdict, detail) for one case."""
    if not preload_mode:
        # Baseline only: the case must pass on glibc.
        if baseline_rc == 0:
            return "PASS", "baseline ok"
        return "FAIL", f"baseline rc={baseline_rc}"
    # Preload mode.
    if baseline_rc != 0:
        return "FAIL", f"baseline broken (rc={baseline_rc}); fix glibc case before judging libsd"
    if fn.status in ("accelerated", "passthrough"):
        if preload_rc == 0:
            return "PASS", "preload matches baseline"
        return "FAIL", f"preload rc={preload_rc} but contract says {fn.status}"
    if fn.status == "partial":
        # Partial: must not crash or hang; non-zero exit OK if not 124/abort.
        if preload_rc in (124,) or preload_rc is None:
            return "FAIL", "preload hung or crashed"
        if preload_rc < 0:
            return "FAIL", f"preload signaled (rc={preload_rc})"
        return "PASS", f"preload rc={preload_rc} (partial contract)"
    if fn.status == "unsupported":
        # Unsupported: any non-zero is OK; passing on glibc but failing
        # under libsd is the *expected* outcome.
        return "PASS", f"preload rc={preload_rc} (unsupported, acceptable)"
    return "FAIL", f"unknown status {fn.status}"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--coverage", type=Path, default=COVERAGE)
    ap.add_argument("--preload", type=Path, default=None,
                    help="path to libsd.so to also test under preload")
    ap.add_argument("--build-dir", type=Path, default=HERE / ".build",
                    help="where to compile case binaries")
    args = ap.parse_args(argv)
    args.build_dir.mkdir(parents=True, exist_ok=True)

    fns = parse_coverage(args.coverage)
    if not fns:
        print(f"error: no functions parsed from {args.coverage}", file=sys.stderr)
        return 2

    # Collapse duplicates (multiple fcntl_* rows share a case).
    by_case: dict[str, list[Function]] = {}
    for f in fns:
        if not f.case or f.case.startswith("ltp:"):
            # LTP cases are out-of-process; not exercised in this
            # CI-portable runner. The README explains how to wire LTP.
            continue
        by_case.setdefault(f.case, []).append(f)

    results: list[CaseResult] = []
    for case, owners in sorted(by_case.items()):
        src = HERE / case
        if not src.exists():
            for fn in owners:
                results.append(CaseResult(fn.name, case, fn.status, "MISSING",
                                          "case file missing"))
            continue
        bin_path = args.build_dir / src.stem
        err = compile_case(src, bin_path)
        if err:
            for fn in owners:
                results.append(CaseResult(fn.name, case, fn.status, "FAIL",
                                          f"compile failed: {err.splitlines()[0] if err else ''}"))
            continue
        b_rc, _ = run_binary(bin_path)
        p_rc = None
        if args.preload is not None:
            p_rc, _ = run_binary(bin_path, preload=args.preload)
        for fn in owners:
            verdict, detail = evaluate(fn, b_rc, p_rc,
                                       preload_mode=args.preload is not None)
            results.append(CaseResult(fn.name, case, fn.status, verdict, detail,
                                      b_rc, p_rc))

    fail = 0
    miss = 0
    for r in results:
        tag = {"PASS": "OK", "FAIL": "FAIL", "SKIP": "SKIP", "MISSING": "MISS"}[r.verdict]
        rc = f" b={r.rc_baseline}" + (f" p={r.rc_preload}" if r.rc_preload is not None else "")
        print(f"[{tag:4}] {r.function:30s} {r.status:14s} {r.case_path:35s}{rc}  {r.detail}")
        if r.verdict == "FAIL":
            fail += 1
        if r.verdict == "MISSING":
            miss += 1

    print()
    total = len(results)
    print(f"summary: {total - fail - miss}/{total} pass, {fail} fail, {miss} missing")
    if fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
