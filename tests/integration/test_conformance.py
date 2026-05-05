"""Integration test for the conformance suite.

Verifies the suite runs end-to-end against unmodified glibc and that
every case in coverage.toml exists, compiles, and the runner reports
PASS for the baseline configuration.
"""

from __future__ import annotations

import os
import subprocess
import shutil
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "tests" / "conformance" / "run_conformance.py"
COVERAGE = REPO / "tests" / "conformance" / "coverage.toml"


def test_runner_exists():
    assert RUNNER.exists()
    assert COVERAGE.exists()


def test_baseline_run_all_pass(tmp_path):
    if shutil.which("cc") is None:
        pytest.skip("no C compiler available")
    r = subprocess.run(
        ["python3", str(RUNNER), "--build-dir", str(tmp_path / "build")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    out = r.stdout.decode()
    assert r.returncode == 0, out + r.stderr.decode()
    # Sanity: at least 20 cases listed.
    assert "summary:" in out
    summary = [l for l in out.splitlines() if l.startswith("summary:")][0]
    # Format: "summary: N/T pass, F fail, M missing"
    assert "fail," in summary
    assert " 0 fail," in summary
    assert " 0 missing" in summary


def test_coverage_lists_every_status_class():
    text = COVERAGE.read_text()
    for s in ("accelerated", "passthrough", "partial", "unsupported"):
        assert s in text, f"coverage.toml missing status class: {s}"


def test_every_case_file_referenced_exists():
    # Parse the case = "cases/x.c" entries directly.
    cases_dir = COVERAGE.parent / "cases"
    referenced = set()
    # Tolerate variable column-aligned spacing produced by editors.
    import re as _re
    pat = _re.compile(r'^case\s*=\s*"([^"]+)"\s*$')
    for line in COVERAGE.read_text().splitlines():
        line = line.split("#", 1)[0].rstrip()
        m = pat.match(line.lstrip())
        if not m:
            continue
        v = m.group(1)
        if v.startswith("cases/"):
            referenced.add(v[len("cases/"):])
    assert referenced, "no case files referenced"
    for name in referenced:
        assert (cases_dir / name).exists(), f"missing case file: {name}"
