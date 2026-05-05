"""Tests for tools/perf_regression.py.

This script is the gate that fails CI when a microbench regresses by
more than the threshold. It must:
  - PASS when current is within tolerance of baseline
  - REGRESS when throughput drops or latency rises by more than the threshold
  - IMPROVE when the change is in the favorable direction
  - Tolerate missing pairs (NEW in current, MISSING in current)
  - Exit non-zero only on regression
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "perf_regression.py"


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    with path.open("w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")


def _run(baseline: Path, current: Path, threshold: float = 0.10):
    return subprocess.run(
        [
            "python3", str(TOOL),
            "--baseline", str(baseline),
            "--current",  str(current),
            "--threshold", str(threshold),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
    )


def test_throughput_within_tolerance_passes(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "throughput", "throughput_mps": 1_000_000}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "throughput", "throughput_mps":   950_000}])  # 5%
    r = _run(base, cur, 0.10)
    assert r.returncode == 0, r.stdout + r.stderr
    assert b"PASS" in r.stdout


def test_throughput_drop_beyond_threshold_regresses(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "throughput", "throughput_mps": 1_000_000}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "throughput", "throughput_mps":   800_000}])  # 20%
    r = _run(base, cur, 0.10)
    assert r.returncode == 1
    assert b"REGRESS" in r.stdout


def test_throughput_improvement_passes(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "throughput", "throughput_mps": 1_000_000}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "throughput", "throughput_mps": 1_500_000}])
    r = _run(base, cur, 0.10)
    assert r.returncode == 0
    assert b"IMPROVED" in r.stdout


def test_latency_increase_regresses(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "latency", "p50_ns": 100}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "latency", "p50_ns": 130}])  # +30%
    r = _run(base, cur, 0.10)
    assert r.returncode == 1
    assert b"REGRESS" in r.stdout


def test_latency_decrease_improves(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "latency", "p50_ns": 100}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "latency", "p50_ns":  70}])
    r = _run(base, cur, 0.10)
    assert r.returncode == 0
    assert b"IMPROVED" in r.stdout


def test_new_metric_does_not_regress(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [])
    _write_jsonl(cur,  [{"bench": "q", "mode": "throughput", "throughput_mps": 100}])
    r = _run(base, cur, 0.10)
    assert r.returncode == 0
    assert b"NEW" in r.stdout


def test_missing_in_current_regresses(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "throughput", "throughput_mps": 100}])
    _write_jsonl(cur,  [])
    r = _run(base, cur, 0.10)
    assert r.returncode == 1
    assert b"MISSING" in r.stdout


def test_threshold_is_respected(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    _write_jsonl(base, [{"bench": "q", "mode": "throughput", "throughput_mps": 1_000_000}])
    _write_jsonl(cur,  [{"bench": "q", "mode": "throughput", "throughput_mps":   850_000}])  # 15%
    # 10% threshold -> regression
    r = _run(base, cur, 0.10)
    assert r.returncode == 1
    # 20% threshold -> pass
    r = _run(base, cur, 0.20)
    assert r.returncode == 0


def test_invalid_input_does_not_silently_pass(tmp_path):
    base = tmp_path / "base.jsonl"
    cur  = tmp_path / "cur.jsonl"
    base.write_text("this is not json\n")
    cur.write_text(json.dumps({"bench": "q", "mode": "throughput", "throughput_mps": 1}) + "\n")
    r = _run(base, cur)
    # Malformed input should crash, not "pass".
    assert r.returncode != 0
