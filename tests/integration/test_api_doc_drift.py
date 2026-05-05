"""CI gate: docs/API.md must be in sync with coverage.toml.

If this test fails, run:
    python3 tools/render_api_doc.py
and commit the resulting docs/API.md.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "render_api_doc.py"
DOC = REPO / "docs" / "API.md"


def test_api_doc_in_sync_with_coverage():
    r = subprocess.run(
        ["python3", str(TOOL), "--check"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
    )
    assert r.returncode == 0, (
        "docs/API.md is stale relative to tests/conformance/coverage.toml.\n"
        "Run: python3 tools/render_api_doc.py\n"
        + r.stdout.decode()
    )


def test_doc_has_auto_table_markers():
    body = DOC.read_text()
    assert "<!-- AUTO-TABLE-BEGIN -->" in body
    assert "<!-- AUTO-TABLE-END -->" in body
