"""Validate that internal markdown links in the docs/ tree resolve.

We don't try to be a full markdown linter — just catch the cheap
mistakes (broken doc cross-links).
"""
from __future__ import annotations
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "docs"

# (`text`)(target) — matches inline markdown links.
LINK_RE = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


def _doc_files() -> list[Path]:
    return sorted(DOCS.glob("*.md")) + [
        REPO / "README.md",
        REPO / "CONTRIBUTING.md",
        REPO / "CHANGELOG.md",
    ]


def test_every_doc_link_resolves():
    failures = []
    for f in _doc_files():
        if not f.exists():
            continue
        body = f.read_text()
        for m in LINK_RE.finditer(body):
            target = m.group(1).split("#", 1)[0]  # strip anchor
            if not target:
                continue
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            # Resolve relative to the file containing the link.
            base = f.parent
            resolved = (base / target).resolve()
            # Allow globs (*.cpp etc.) that we use in CONTRIBUTING.
            if "*" in target:
                continue
            if not resolved.exists():
                failures.append(f"{f.relative_to(REPO)}: broken link → {target}")
    assert not failures, "\n".join(failures)


def test_docs_index_lists_every_top_level_doc():
    """docs/README.md is the entry point; make sure it actually mentions
    every doc under docs/ so newcomers don't miss anything."""
    index = (DOCS / "README.md").read_text()
    for f in DOCS.glob("*.md"):
        if f.name in ("README.md",):
            continue
        # The link target may be `f.name` or `f.stem` (without `.md`)
        # depending on the doc — accept either.
        ok = (
            f.name in index
            or f.stem in index
        )
        assert ok, f"docs/README.md does not reference docs/{f.name}"
