#!/usr/bin/env python3
"""check_kernel_abi.py — fail if the kernel-side and userspace-side
zero-copy ABI definitions have drifted apart.

Phase 5 deliberately keeps two copies of the struct/ioctl definitions:
one in include/socksdirect/zerocopy.h (compiled by the userspace lib +
the unit tests) and one in src/kernel/socksdirect_dev.h (compiled by
the kernel module). The kernel build can't easily reach into the
userspace include tree, so we accept the duplication and police it
with this CI check.

What we compare (textually, after stripping comments + types):
  - Field names of every `struct sd_*` in both files.
  - The IOCTL macro list (`SD_IOC_*`).
  - The MAJOR/MINOR/MAX_ORDER constants.

Exits 0 on match, 1 with a diff on drift.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
USER_HEADER = REPO / "include" / "socksdirect" / "zerocopy.h"
KERN_HEADER = REPO / "src" / "kernel" / "socksdirect_dev.h"

STRUCT_RE = re.compile(r"struct\s+(sd_\w+)\s*\{([^}]*)\}", re.DOTALL)
FIELD_RE  = re.compile(r"\b(__?u\d+|uint\d+_t)\s+(\w+)\s*;")
IOCTL_RE  = re.compile(r"#define\s+(SD_IOC_\w+)\s+\S")
CONST_RE  = re.compile(r"#define\s+(SOCKSDIRECT_(?:ABI_(?:MAJOR|MINOR)|MAX_ORDER))\s+(\S+)")


def parse(path: Path) -> dict:
    text = path.read_text()
    structs = {}
    for m in STRUCT_RE.finditer(text):
        name = m.group(1)
        body = m.group(2)
        fields = [(t, n) for (t, n) in FIELD_RE.findall(body)]
        structs[name] = [n for (_, n) in fields]
    ioctls = sorted({m.group(1) for m in IOCTL_RE.finditer(text)})
    consts = {m.group(1): m.group(2) for m in CONST_RE.finditer(text)}
    return {"structs": structs, "ioctls": ioctls, "consts": consts}


def diff(label, a, b) -> list[str]:
    out = []
    if a != b:
        out.append(f"{label} mismatch:")
        out.append(f"  user: {a}")
        out.append(f"  kern: {b}")
    return out


def main() -> int:
    u = parse(USER_HEADER)
    k = parse(KERN_HEADER)
    issues: list[str] = []

    # Struct presence + field-order match
    user_structs = set(u["structs"])
    kern_structs = set(k["structs"])
    if user_structs != kern_structs:
        issues.append(
            "struct set mismatch:\n"
            f"  only in user: {sorted(user_structs - kern_structs)}\n"
            f"  only in kern: {sorted(kern_structs - user_structs)}"
        )
    for name in sorted(user_structs & kern_structs):
        issues += diff(f"fields of struct {name}",
                       u["structs"][name], k["structs"][name])

    # Ioctl macro set
    issues += diff("ioctl macro list", u["ioctls"], k["ioctls"])

    # Version constants
    issues += diff("constants", u["consts"], k["consts"])

    if issues:
        print("kernel-userspace ABI drift detected:")
        for line in issues:
            print("  " + line)
        return 1
    print("ABI: user and kernel definitions are in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
