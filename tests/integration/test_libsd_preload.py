"""End-to-end tests for the new src/lib/ libsd preload library.

Validates the post-rewrite library:
  - Loads cleanly via LD_PRELOAD.
  - Doesn't break basic libc-using programs.
  - Tracks fds via FdRemapTable (visible via metrics scrape).
  - Implements dup/dup2/dup3 / shutdown / fork correctly under preload.
  - Connects to the monitor when present; tolerates its absence.
"""
from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
LIB  = REPO / "build" / "libsd.so"


def _need_libsd() -> Path:
    if not LIB.exists():
        pytest.skip("libsd.so not built")
    return LIB


def _preload_env(extra: dict = None) -> dict:
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIB)
    env.setdefault("SOCKSDIRECT_LOG", "warn")
    if extra:
        env.update(extra)
    return env


def _run_under_preload(*argv, timeout=10, env_extra=None):
    return subprocess.run(
        argv,
        env=_preload_env(env_extra),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


# ---------------------------------------------------------------------------
# Smoke tests — basic libc programs survive preload.
# ---------------------------------------------------------------------------

def test_true_returns_zero():
    _need_libsd()
    r = _run_under_preload("/bin/true")
    assert r.returncode == 0, r.stderr


def test_echo_passes_through():
    _need_libsd()
    r = _run_under_preload("/bin/echo", "hello")
    assert r.returncode == 0
    assert r.stdout.strip() == b"hello"


def test_ls_works():
    _need_libsd()
    r = _run_under_preload("/bin/ls", "/")
    assert r.returncode == 0
    assert b"usr" in r.stdout


def test_curl_against_python_http_server(tmp_path):
    """Round-trip a real HTTP GET through preloaded curl."""
    _need_libsd()
    if not shutil.which("curl"):
        pytest.skip("curl not installed")
    # Pick a free port ourselves.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    docroot = tmp_path / "www"
    docroot.mkdir()
    (docroot / "index.html").write_text("ok")
    srv = subprocess.Popen(
        ["python3", "-m", "http.server", str(port), "--bind", "127.0.0.1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=str(docroot),
    )
    # Spin until the port is accepting.
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.1):
                break
        except OSError:
            time.sleep(0.05)
    else:
        srv.terminate(); srv.wait()
        pytest.skip("python http.server did not come up")
    try:
        r = _run_under_preload("curl", "-sS", f"http://127.0.0.1:{port}/")
        assert r.returncode == 0, r.stderr
        assert r.stdout.startswith(b"ok")
    finally:
        srv.terminate(); srv.wait(timeout=5)


# ---------------------------------------------------------------------------
# Conformance suite under preload — every coverage.toml entry runs against
# the libsd-instrumented path and matches its declared status contract.
# ---------------------------------------------------------------------------

def test_conformance_under_preload():
    _need_libsd()
    r = subprocess.run(
        [
            "python3",
            str(REPO / "tests" / "conformance" / "run_conformance.py"),
            "--preload", str(LIB),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=180,
    )
    assert r.returncode == 0, r.stdout.decode()


# ---------------------------------------------------------------------------
# Programmatic tests — exercise specific Phase 3 fixes from a child run
# under preload. Each test compiles a tiny C program then runs it.
# ---------------------------------------------------------------------------

CASES = REPO / "tests" / "conformance" / "cases"


def _compile(src: Path, dst: Path):
    r = subprocess.run(
        ["cc", "-O0", "-D_GNU_SOURCE", "-o", str(dst), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()


def test_dup_family_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "dup"
    _compile(CASES / "dup_unsupported.c", bin_path)
    r = _run_under_preload(str(bin_path))
    assert r.returncode == 0, r.stderr  # was UNSUPPORTED in the prototype


def test_shutdown_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "shutdown"
    _compile(CASES / "shutdown_unsupported.c", bin_path)
    r = _run_under_preload(str(bin_path))
    assert r.returncode == 0, r.stderr  # was silently ignored before


def test_fork_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "fork"
    _compile(CASES / "fork_basic.c", bin_path)
    r = _run_under_preload(str(bin_path))
    assert r.returncode == 0, r.stderr


def test_vfork_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "vfork"
    _compile(CASES / "vfork_unsupported.c", bin_path)
    r = _run_under_preload(str(bin_path))
    assert r.returncode == 0, r.stderr  # was UNSUPPORTED before


def test_clone_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "clone"
    _compile(CASES / "clone_unsupported.c", bin_path)
    r = _run_under_preload(str(bin_path))
    assert r.returncode == 0, r.stderr


def test_accept_basic_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "accept"
    _compile(CASES / "accept_basic.c", bin_path)
    r = _run_under_preload(str(bin_path), timeout=15)
    assert r.returncode == 0, r.stderr


def test_send_recv_under_preload(tmp_path):
    _need_libsd()
    bin_path = tmp_path / "sr"
    _compile(CASES / "send_recv_basic.c", bin_path)
    r = _run_under_preload(str(bin_path), timeout=15)
    assert r.returncode == 0, r.stderr


# ---------------------------------------------------------------------------
# Monitor integration — libsd opens the control socket if present.
# ---------------------------------------------------------------------------

def test_libsd_pings_monitor_when_present(tmp_path):
    _need_libsd()
    monitor = REPO / "build" / "socksdirect-monitor"
    if not monitor.exists():
        pytest.skip("monitor not built")
    sock = tmp_path / "ctl.sock"
    pid_file = tmp_path / "m.pid"
    p = subprocess.Popen(
        [str(monitor),
         "--control-socket", str(sock),
         "--pid-file", str(pid_file),
         "--log-level", "warn"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    deadline = time.time() + 5
    while time.time() < deadline and not sock.exists():
        time.sleep(0.02)
    assert sock.exists(), "monitor failed to bind"
    try:
        env = _preload_env({
            "SOCKSDIRECT_MONITOR_CONTROL_SOCKET": str(sock),
        })
        # Run a child under preload; libsd's constructor should ping.
        subprocess.run(
            ["/bin/true"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=True,
        )
        # Ask the monitor for its metrics; ctl_requests_total should have
        # ticked at least once for the ping.
        ctl = REPO / "build" / "socksdirect-ctl"
        env2 = os.environ.copy()
        env2["SOCKSDIRECT_CTL_SOCKET"] = str(sock)
        r = subprocess.run(
            [str(ctl), "metrics"],
            env=env2, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
        )
        assert r.returncode == 0
        line = next(
            (l for l in r.stdout.decode().splitlines()
             if l.startswith("socksdirect_ctl_requests_total ")),
            "",
        )
        assert line, r.stdout
        assert float(line.split()[1]) >= 2  # ping + this metrics
    finally:
        p.send_signal(signal.SIGTERM)
        p.wait(timeout=5)


def test_libsd_works_without_monitor():
    _need_libsd()
    env = _preload_env({"SOCKSDIRECT_MONITOR_CONTROL_SOCKET": "/tmp/nope.sock"})
    r = subprocess.run(
        ["/bin/true"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
    )
    assert r.returncode == 0
