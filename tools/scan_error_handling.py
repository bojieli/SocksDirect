#!/usr/bin/env python3
"""scan_error_handling.py — locate FATAL/ERROR/assert/abort sites that
need converting from "crash on user input" to "errno return".

Phase 3 of REWRITE_PLAN.md mandates that user-input paths return
errno-style failures rather than aborting the process. The legacy
tree (common/, lib/, monitor/) is full of `FATAL(...)` and `assert(...)`
calls that originated from the research prototype, where a misbehaving
caller crashed the test driver instead of being handled.

This script enumerates every such site, classifies it by call macro,
and emits a Markdown table sorted by file:line. The output is the
auto-generated body of docs/error-handling-audit.md.

Classification heuristic (best-effort; a human still owns the
disposition column):
  - assert(...)           → potentially user-input; needs review
  - FATAL(...)            → almost always exit-on-bad-input; convert
  - abort()               → exit-on-condition; convert
  - exit(N)/_exit(N)      → CLI tool exits, not user-input paths
  - ERROR(...)/DEBUG(...) → already errno paths; informational

Sites already converted (or off the user-input path) get tagged
DONE in coverage_overrides below; the audit doc shows them under a
"Resolved" footer.

Usage:
  scan_error_handling.py                # write to docs/error-handling-audit.md
  scan_error_handling.py --check        # exit 1 if doc is stale
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

REPO = Path(__file__).resolve().parents[1]
DOC = REPO / "docs" / "error-handling-audit.md"
SCAN_ROOTS = [REPO / "lib", REPO / "monitor", REPO / "common"]
PATTERN = re.compile(
    r'\b('
    r'FATAL'
    r'|assert'
    r'|abort'
    r')\s*\('
)

BEGIN_MARKER = "<!-- AUTO-AUDIT-BEGIN -->"
END_MARKER   = "<!-- AUTO-AUDIT-END -->"

# Sites listed here are excluded from the table — they're either in
# infrastructure that genuinely should crash on internal corruption
# (the locklessq sizeof asserts, for instance) or already in
# converted code paths.
RESOLVED = {
    # (file relative to repo, line, kind)
    # Add entries here as you fix sites.
}


@dataclass
class Site:
    path: Path        # absolute
    rel: str          # repo-relative
    line: int
    kind: str         # FATAL / assert / abort
    text: str         # full line, trimmed


def scan() -> list[Site]:
    out: list[Site] = []
    for root in SCAN_ROOTS:
        if not root.exists():
            continue
        for f in sorted(root.rglob("*")):
            if f.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp"):
                continue
            if "__deprecated_" in f.name:
                continue
            try:
                lines = f.read_text(errors="replace").splitlines()
            except OSError:
                continue
            for i, line in enumerate(lines, start=1):
                # Skip pure-comment lines (starts with // or *).
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                # Skip macro definitions (#define).
                if stripped.startswith("#define"):
                    continue
                m = PATTERN.search(line)
                if not m:
                    continue
                # static_assert / assertion-like compile-time things — skip.
                if "static_assert" in line:
                    continue
                kind = m.group(1)
                rel = str(f.relative_to(REPO))
                key = (rel, i, kind)
                if key in RESOLVED:
                    continue
                out.append(Site(
                    path=f, rel=rel, line=i, kind=kind,
                    text=line.strip()[:120],
                ))
    return out


def render(sites: list[Site]) -> str:
    out: list[str] = []
    out.append(BEGIN_MARKER)
    out.append("")
    out.append(
        "_Auto-generated from `tools/scan_error_handling.py`. "
        "Re-run on every PR that touches `lib/`, `monitor/`, or `common/`._"
    )
    out.append("")

    by_file: dict[str, list[Site]] = {}
    for s in sites:
        by_file.setdefault(s.rel, []).append(s)

    out.append(f"**Total sites:** {len(sites)} across {len(by_file)} file(s).")
    out.append("")
    by_kind: dict[str, int] = {}
    for s in sites:
        by_kind[s.kind] = by_kind.get(s.kind, 0) + 1
    out.append("| Kind | Count |")
    out.append("|---|---|")
    for k in sorted(by_kind):
        out.append(f"| `{k}(...)` | {by_kind[k]} |")
    out.append("")

    for rel in sorted(by_file):
        out.append(f"### {rel}")
        out.append("")
        out.append("| Line | Kind | Excerpt |")
        out.append("|---|---|---|")
        for s in sorted(by_file[rel], key=lambda x: x.line):
            excerpt = s.text.replace("|", "\\|")
            out.append(f"| {s.line} | `{s.kind}` | `{excerpt}` |")
        out.append("")

    out.append(END_MARKER)
    return "\n".join(out)


def splice(existing: str, generated: str) -> str:
    if BEGIN_MARKER in existing and END_MARKER in existing:
        b = existing.index(BEGIN_MARKER)
        e = existing.index(END_MARKER) + len(END_MARKER)
        return existing[:b] + generated + existing[e:]
    return existing.rstrip() + "\n\n" + generated + "\n"


PREAMBLE = """\
# Error-handling audit (Phase 3)

Phase 3 of `REWRITE_PLAN.md` mandates that **user-input paths return
errno-style failures**, not crash the process. `FATAL(...)` is reserved
for detected internal corruption; `assert(...)` is for invariants that
external input cannot violate; `abort()` should be exceptional.

This document enumerates every site in the legacy tree (`common/`,
`lib/`, `monitor/`) where the prototype currently exits or asserts on
something the user can drive into. Each entry needs a disposition:

- **CONVERT** → swap to `errno = X; return -1;` (or equivalent).
- **KEEP** → genuinely internal-only invariant.
- **MOVE** → asserts that should become `static_assert`.

The table below is auto-generated by `tools/scan_error_handling.py`
on every PR; the disposition column lives outside the marker and is
maintained by hand as fixes land.

## Disposition history

| Date | Site | Disposition | Phase 3 PR |
|---|---|---|---|
| _(none yet)_ | | | |

"""


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=DOC)
    p.add_argument("--check", action="store_true")
    args = p.parse_args(argv)

    sites = scan()
    block = render(sites)

    existing = args.out.read_text() if args.out.exists() else PREAMBLE
    if BEGIN_MARKER not in existing:
        existing = PREAMBLE
    new_text = splice(existing, block)

    if args.check:
        if existing != new_text:
            print(f"{args.out} is out of sync with the legacy tree")
            print("Run: tools/scan_error_handling.py")
            return 1
        print(f"{args.out} is in sync.")
        return 0

    if existing == new_text:
        print(f"{args.out} already up to date")
        return 0
    args.out.write_text(new_text)
    print(f"wrote {args.out} ({len(sites)} sites)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
