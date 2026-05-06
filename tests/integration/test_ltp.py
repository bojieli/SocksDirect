"""LTP socket conformance — runs both baseline and preload variants.

When LTP isn't installed, both tests skip cleanly. The driver script
itself must exist and be runnable, so the test collection is real.
"""
from __future__ import annotations
import subprocess
from pathlib import Path
import os
import pytest

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / "tests" / "conformance" / "ltp.py"
LIB = REPO / "build" / "libsd.so"


def test_driver_runs_and_handles_missing_ltp():
    r = subprocess.run(
        ["python3", str(DRIVER)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60,
    )
    # 0 is "everything either passed or LTP isn't installed".
    assert r.returncode == 0, r.stdout + r.stderr


def test_under_preload_when_lib_built():
    if not LIB.exists():
        pytest.skip("libsd.so not built")
    r = subprocess.run(
        ["python3", str(DRIVER), "--preload", str(LIB)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    assert r.returncode == 0, r.stdout
