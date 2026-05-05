#!/usr/bin/env python3
"""render_api_doc.py — generate docs/API.md from coverage.toml.

The conformance coverage matrix at tests/conformance/coverage.toml is
the single source of truth for which libc functions libsd intercepts
and what level of support each one has. This script reads that file
and emits a Markdown document summarizing the API surface, ready to
drop into docs/API.md.

CI uses this in two modes:
  - default     : write docs/API.md (or whatever --out points to).
  - --check     : compare what would be generated to the on-disk doc;
                  exit 1 with a unified diff if they differ.

Why we keep both files: the on-disk doc has a hand-written prologue
and epilogue (the "How to read this table" / "Reporting a missing
function" sections) that don't belong in coverage.toml. The script
preserves anything between the explicit BEGIN/END markers; only the
table itself is generated.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

REPO = Path(__file__).resolve().parents[1]
COVERAGE = REPO / "tests" / "conformance" / "coverage.toml"
DOC = REPO / "docs" / "API.md"

BEGIN_MARKER = "<!-- AUTO-TABLE-BEGIN -->"
END_MARKER   = "<!-- AUTO-TABLE-END -->"


@dataclass
class Function:
    name: str
    status: str = ""
    case: str = ""
    fix_phase: Optional[int] = None
    notes: str = ""


_SCALAR = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"?([^"#]*?)"?\s*(?:#.*)?$')


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
        m = _SCALAR.match(s)
        if not m or cur is None:
            continue
        k, v = m.group(1), m.group(2).strip()
        if k == "fix_phase":
            cur.fix_phase = int(v)
        else:
            setattr(cur, k, v)
    if cur is not None:
        out.append(cur)
    return out


# Functions are grouped into sections by name prefix. Tweak this map to
# expand a category; unmatched names land in "Other".
SECTIONS = [
    ("Sockets", lambda n: n in {
        "socket", "bind", "listen", "accept", "accept4", "connect",
        "send", "recv", "sendto", "recvfrom", "sendmsg", "recvmsg",
        "shutdown", "getsockopt", "setsockopt", "getsockname", "getpeername",
        "socketpair", "sendfile",
    }),
    ("Multiplexing", lambda n: n in {
        "epoll_create", "epoll_create1", "epoll_ctl", "epoll_wait",
        "epoll_pwait", "select", "pselect", "poll", "ppoll",
    }),
    ("File descriptors", lambda n: n.startswith("fcntl") or n in {
        "close", "dup", "dup2", "dup3", "ioctl",
    }),
    ("Process lifecycle", lambda n: n in {
        "fork", "vfork", "clone", "pthread_create", "exec", "daemon", "sigaction",
    }),
    ("Files", lambda n: n in {
        "open", "openat", "creat", "read", "write", "pread", "pwrite",
        "mmap", "munmap",
    }),
]


def render(funcs: list[Function]) -> str:
    out: list[str] = []
    out.append(BEGIN_MARKER)
    out.append("")
    out.append("_The table below is generated from "
               "[tests/conformance/coverage.toml](../tests/conformance/coverage.toml). "
               "Edit that file (and the matching case under "
               "`tests/conformance/cases/`) instead of editing this section by hand. "
               "CI fails on drift._")
    out.append("")
    bucketed: dict[str, list[Function]] = {}
    for f in funcs:
        sect = "Other"
        for name, predicate in SECTIONS:
            if predicate(f.name):
                sect = name
                break
        bucketed.setdefault(sect, []).append(f)

    for sect_name, _ in SECTIONS + [("Other", lambda _n: True)]:
        rows = bucketed.get(sect_name, [])
        if not rows:
            continue
        out.append(f"### {sect_name}")
        out.append("")
        out.append("| Function | Status | Notes |")
        out.append("|---|---|---|")
        for f in sorted(rows, key=lambda r: r.name):
            note = f.notes.strip()
            if f.fix_phase is not None:
                tag = f"**Phase {f.fix_phase} fix.**"
                note = (note + " " + tag).strip()
            row = f"| `{f.name}` | {f.status.upper()} | {note} |"
            out.append(row)
        out.append("")
    out.append(END_MARKER)
    return "\n".join(out)


def splice(existing: str, generated_block: str) -> str:
    """Replace the section between BEGIN and END markers with the
    freshly generated block. If the markers don't exist yet, append
    them (for first-time setup)."""
    if BEGIN_MARKER in existing and END_MARKER in existing:
        b = existing.index(BEGIN_MARKER)
        e = existing.index(END_MARKER) + len(END_MARKER)
        return existing[:b] + generated_block + existing[e:]
    return existing.rstrip() + "\n\n" + generated_block + "\n"


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--coverage", type=Path, default=COVERAGE)
    p.add_argument("--out", type=Path, default=DOC)
    p.add_argument("--check", action="store_true",
                   help="don't write; exit 1 with a diff if --out is stale")
    args = p.parse_args(argv)

    funcs = parse_coverage(args.coverage)
    if not funcs:
        print(f"error: no functions parsed from {args.coverage}", file=sys.stderr)
        return 2
    block = render(funcs)
    existing = args.out.read_text() if args.out.exists() else ""
    new_text = splice(existing, block)

    if args.check:
        if existing != new_text:
            print(f"{args.out} is out of sync with {args.coverage}")
            print("Run: tools/render_api_doc.py")
            print()
            sys.stdout.writelines(difflib.unified_diff(
                existing.splitlines(keepends=True),
                new_text.splitlines(keepends=True),
                fromfile=str(args.out) + " (on disk)",
                tofile=str(args.out)   + " (generated)",
            ))
            return 1
        print(f"{args.out} is in sync with {args.coverage}")
        return 0

    if existing == new_text:
        print(f"{args.out} already up to date")
        return 0
    args.out.write_text(new_text)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
