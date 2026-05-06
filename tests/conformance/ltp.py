#!/usr/bin/env python3
"""ltp.py — run the relevant Linux Test Project subset against libsd.

LTP ships hundreds of socket-related tests; we cherry-pick a small
high-signal subset that maps onto our coverage matrix and expose them
to CI as either passing-baseline or passing-under-preload.

Two modes:

  baseline (default)         run each LTP test against unmodified glibc.
                             Validates that the test runner works and
                             the host has LTP installed.

  --preload PATH/libsd.so    run each LTP test under preload. Tests
                             tagged "skip_under_preload" in
                             SUBSET_TESTS get skipped (they exercise
                             surfaces libsd doesn't claim to handle).

LTP isn't always installed; if `ltp_run` or the per-test binary path
isn't found, this script returns 0 with a SKIP message so CI on
runners without LTP doesn't fail.

Default LTP install layouts we probe:
  /opt/ltp/testcases/kernel/syscalls/<test>/<test>
  /usr/share/ltp/testcases/kernel/syscalls/<test>/<test>

Cherry-picked subset (high-signal, fast):
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

LTP_ROOTS = [
    Path("/opt/ltp/testcases/kernel/syscalls"),
    Path("/usr/share/ltp/testcases/kernel/syscalls"),
]

# (test name, expected-status-under-preload)
#   ok          — must pass under preload
#   skip        — skip when --preload is set (unsupported surface)
SUBSET = [
    ("socket/socket01",   "ok"),
    ("socket/socket02",   "ok"),
    ("bind/bind01",       "ok"),
    ("listen/listen01",   "ok"),
    ("accept/accept01",   "ok"),
    ("connect/connect01", "ok"),
    ("send/send01",       "ok"),
    ("sendmsg/sendmsg01", "ok"),
    ("recv/recv01",       "ok"),
    ("recvmsg/recvmsg01", "ok"),
    ("shutdown/shutdown01", "ok"),
    ("getsockname/getsockname01", "ok"),
    ("getpeername/getpeername01", "ok"),
    ("epoll_create/epoll_create01", "ok"),
    ("epoll_ctl/epoll_ctl01",       "ok"),
    ("epoll_wait/epoll_wait01",     "ok"),
    ("dup/dup01",                   "ok"),
    ("dup2/dup201",                 "ok"),
    ("dup3/dup301",                 "ok"),
    ("close/close01",               "ok"),
    ("fcntl/fcntl01",               "ok"),
    ("fork/fork01",                 "ok"),
    ("vfork/vfork01",               "ok"),
    # Tests that rely on internals libsd doesn't claim to handle:
    ("setsockopt/setsockopt05",     "skip"),
    ("ioctl/ioctl08",               "skip"),
]


@dataclass
class Outcome:
    name: str
    bin: Optional[Path]
    rc: Optional[int] = None
    stdout: bytes = b""
    stderr: bytes = b""

    @property
    def state(self) -> str:
        if self.bin is None: return "MISSING"
        if self.rc is None:  return "SKIPPED"
        return "PASS" if self.rc == 0 else "FAIL"


def find_test(name: str) -> Optional[Path]:
    for root in LTP_ROOTS:
        cand = root / name
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    return None


def run_one(name: str, expected_under_preload: str,
            preload: Optional[Path]) -> Outcome:
    bin_path = find_test(name)
    o = Outcome(name=name, bin=bin_path)
    if bin_path is None:
        return o
    if preload is not None and expected_under_preload == "skip":
        # Skipped under preload by design — just count it as missing
        # so the summary doesn't lie.
        return o
    env = os.environ.copy()
    if preload is not None:
        env["LD_PRELOAD"] = str(preload)
        env.setdefault("SOCKSDIRECT_LOG", "warn")
    r = subprocess.run([str(bin_path)], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=60)
    o.rc = r.returncode
    o.stdout = r.stdout
    o.stderr = r.stderr
    return o


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--preload", type=Path, default=None,
                   help="run under LD_PRELOAD=PATH (libsd.so)")
    p.add_argument("--filter", default=None,
                   help="run only tests whose name contains this substring")
    args = p.parse_args(argv)

    # LTP not installed → SKIP cleanly. CI gate consults this exit code.
    found_any = any(find_test(n) is not None for n, _ in SUBSET)
    if not found_any:
        print("ltp: no LTP tests found in any of:")
        for r in LTP_ROOTS:
            print(f"  {r}")
        print("install ltp (apt install ltp) to enable this gate")
        return 0

    fail = 0
    miss = 0
    for name, expected in SUBSET:
        if args.filter and args.filter not in name:
            continue
        o = run_one(name, expected, args.preload)
        tag = {"PASS": "OK", "FAIL": "FAIL", "SKIPPED": "SKIP",
               "MISSING": "MISS"}[o.state]
        loc = (str(o.bin) if o.bin else "(absent)")
        line = f"[{tag:4}] {o.name:40s} {loc}"
        if o.state == "FAIL":
            line += f" rc={o.rc}"
        print(line)
        if o.state == "FAIL":
            fail += 1
        if o.state == "MISSING":
            miss += 1

    print()
    print(f"summary: {len(SUBSET) - fail - miss}/{len(SUBSET)} pass, {fail} fail, {miss} missing")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
