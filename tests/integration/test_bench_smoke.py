"""Smoke tests for the microbenchmark binaries.

We don't compare to a baseline here (that's the perf-regression gate's
job, run on a self-hosted runner). We just verify each binary launches,
exits cleanly, and emits parseable JSON in the schema the perf gate
expects.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "build" / "bench" / "microbench"

REQUIRED_KEYS = {"bench", "mode", "msg_count", "elapsed_ns", "throughput_mps", "p50_ns"}


def _run(name: str, *args, timeout: int = 20):
    path = BENCH_DIR / name
    if not path.exists():
        pytest.skip(f"{name} binary not built")
    return subprocess.run(
        [str(path), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def _validate_lines(stdout: bytes) -> list[dict]:
    rows = []
    for line in stdout.decode().splitlines():
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        missing = REQUIRED_KEYS - set(obj.keys())
        assert not missing, f"missing keys {missing}: {obj}"
        rows.append(obj)
    assert rows, "expected at least one JSON line"
    return rows


def test_bench_queue_v3_throughput_emits_json():
    r = _run("bench_queue_v3", "--mode=throughput", "--count=10000")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    assert any(o["mode"] == "throughput" for o in rows)


def test_bench_queue_v3_latency_emits_json():
    r = _run("bench_queue_v3", "--mode=latency", "--count=2000")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    assert any(o["mode"] == "latency" and o["p50_ns"] is not None for o in rows)


def test_bench_fd_remap_emits_per_submode():
    r = _run("bench_fd_remap", "--mode=throughput", "--count=10000", "--threads=2")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    submodes = {o.get("submode") for o in rows}
    # We register at least alloc/lookup/free.
    assert {"alloc", "lookup", "free"}.issubset(submodes)


def test_bench_fd_remap_latency():
    r = _run("bench_fd_remap", "--mode=latency", "--count=2000")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    assert any(o["mode"] == "latency" and o["p50_ns"] is not None for o in rows)


def test_bench_metrics_emits_st_and_mt():
    r = _run("bench_metrics", "--mode=throughput", "--count=10000", "--threads=2")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    submodes = {o.get("submode") for o in rows}
    assert {"counter_inc_st", "counter_inc_mt", "render"}.issubset(submodes)


def test_bench_config_emits_three_paths():
    r = _run("bench_config", "--count=10000")
    assert r.returncode == 0, r.stderr
    rows = _validate_lines(r.stdout)
    submodes = {o.get("submode") for o in rows}
    assert {"get_string_hit", "get_string_miss", "get_int"}.issubset(submodes)


def test_unknown_mode_exits_nonzero():
    r = _run("bench_queue_v3", "--mode=zzz", "--count=10")
    assert r.returncode != 0
