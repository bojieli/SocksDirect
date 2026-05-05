"""CI gate: docs/error-handling-audit.md must reflect the legacy tree.

Re-run tools/scan_error_handling.py to refresh.
"""
from __future__ import annotations
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "scan_error_handling.py"


def test_audit_doc_in_sync():
    r = subprocess.run(
        ["python3", str(TOOL), "--check"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
    )
    assert r.returncode == 0, (
        "docs/error-handling-audit.md is stale. Run:\n"
        "  python3 tools/scan_error_handling.py\n"
        + r.stdout.decode()
    )
