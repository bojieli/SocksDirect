"""Integration tests for the socksdirect-ctl CLI.

Strategy: run a tiny in-process Python "fake monitor" on a temp Unix
socket, point socksdirect-ctl at it via SOCKSDIRECT_CTL_SOCKET, and
assert on the parsed response. This keeps the test CI-portable — no
real monitor, no RDMA, no kernel module.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import threading
from pathlib import Path

import pytest


# ---------------------------------------------------------------------------
# Fake monitor: a single-threaded Unix-socket server that reads one NDJSON
# request per connection and writes one NDJSON response.
# ---------------------------------------------------------------------------

class FakeMonitor:
    def __init__(self, sock_path: Path, handler):
        self.sock_path = sock_path
        self.handler = handler
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if sock_path.exists():
            sock_path.unlink()
        self.srv.bind(str(sock_path))
        self.srv.listen(4)
        self._stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self._stop = True
        try:
            # Kick the accept() loop.
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.connect(str(self.sock_path))
        except OSError:
            pass
        self.srv.close()
        self.thread.join(timeout=2)
        if self.sock_path.exists():
            self.sock_path.unlink()

    def _serve(self):
        while not self._stop:
            try:
                self.srv.settimeout(0.5)
                conn, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            try:
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                line = data.decode("utf-8").strip()
                if not line:
                    conn.close()
                    continue
                req = json.loads(line)
                resp = self.handler(req)
                conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
            except Exception as e:  # noqa: BLE001
                err = {"ok": False, "error": f"server-error: {e}"}
                try:
                    conn.sendall((json.dumps(err) + "\n").encode("utf-8"))
                except OSError:
                    pass
            finally:
                conn.close()


# ---------------------------------------------------------------------------
# Locating socksdirect-ctl
# ---------------------------------------------------------------------------

def _find_ctl() -> Path:
    explicit = os.environ.get("SOCKSDIRECT_CTL")
    if explicit and Path(explicit).exists():
        return Path(explicit)
    repo = Path(__file__).resolve().parents[2]
    for cand in (repo / "build" / "socksdirect-ctl",
                 repo / "build" / "tools" / "socksdirect-ctl"):
        if cand.exists():
            return cand
    pytest.skip("socksdirect-ctl binary not found; build it first")


@pytest.fixture
def ctl_path() -> Path:
    return _find_ctl()


@pytest.fixture
def ctl_socket(tmp_path: Path) -> Path:
    return tmp_path / "ctl.sock"


def _run_ctl(ctl: Path, sock: Path, *args, env_extra=None, timeout=5):
    env = os.environ.copy()
    env["SOCKSDIRECT_CTL_SOCKET"] = str(sock)
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        [str(ctl), *args],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_ok_response_prints_lines_and_exits_0(ctl_path, ctl_socket):
    def handler(req):
        return {"ok": True, "lines": [f"op={req['op']}", f"argc={len(req['args'])}"]}

    with FakeMonitor(ctl_socket, handler):
        r = _run_ctl(ctl_path, ctl_socket, "status")
    assert r.returncode == 0, r.stderr
    out = r.stdout.decode().splitlines()
    assert out == ["op=status", "argc=0"]


def test_args_are_forwarded(ctl_path, ctl_socket):
    seen = {}

    def handler(req):
        seen.update(req)
        return {"ok": True, "lines": []}

    with FakeMonitor(ctl_socket, handler):
        r = _run_ctl(ctl_path, ctl_socket, "connections", "fmt=json", "limit=10")
    assert r.returncode == 0
    assert seen["op"] == "connections"
    assert seen["args"] == ["fmt=json", "limit=10"]


def test_error_response_prints_to_stderr_exits_1(ctl_path, ctl_socket):
    def handler(req):
        return {"ok": False, "error": "no such op: " + req["op"]}

    with FakeMonitor(ctl_socket, handler):
        r = _run_ctl(ctl_path, ctl_socket, "wat")
    assert r.returncode == 1
    assert b"no such op: wat" in r.stderr


def test_no_server_exits_3(ctl_path, tmp_path):
    nope = tmp_path / "absent.sock"
    r = _run_ctl(ctl_path, nope, "status")
    assert r.returncode == 3
    assert b"cannot connect" in r.stderr


def test_malformed_response_exits_4(ctl_path, ctl_socket):
    # Server that sends garbage instead of NDJSON.
    if ctl_socket.exists():
        ctl_socket.unlink()
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(ctl_socket))
    srv.listen(1)
    server_done = threading.Event()

    def serve():
        c, _ = srv.accept()
        c.recv(4096)
        c.sendall(b"not-json-at-all\n")
        c.close()
        server_done.set()

    t = threading.Thread(target=serve, daemon=True)
    t.start()

    try:
        r = _run_ctl(ctl_path, ctl_socket, "status")
        assert r.returncode == 4
    finally:
        server_done.wait(timeout=2)
        srv.close()
        ctl_socket.unlink(missing_ok=True)


def test_help_prints_usage(ctl_path, ctl_socket):
    r = _run_ctl(ctl_path, ctl_socket, "--help")
    assert r.returncode == 0
    assert b"usage:" in r.stdout
    assert b"status" in r.stdout
    assert b"reload" in r.stdout


def test_no_args_exits_2_with_usage(ctl_path, ctl_socket):
    r = _run_ctl(ctl_path, ctl_socket)
    assert r.returncode == 2
    assert b"usage:" in r.stderr


def test_unknown_flag_exits_2(ctl_path, ctl_socket):
    r = _run_ctl(ctl_path, ctl_socket, "--zzz")
    assert r.returncode == 2
    assert b"unknown flag" in r.stderr


def test_socket_flag_overrides_env(ctl_path, ctl_socket, tmp_path):
    # --socket should win over env var.
    decoy = tmp_path / "decoy.sock"
    seen = {}

    def handler(req):
        seen["op"] = req["op"]
        return {"ok": True, "lines": ["from-real"]}

    with FakeMonitor(ctl_socket, handler):
        env = os.environ.copy()
        env["SOCKSDIRECT_CTL_SOCKET"] = str(decoy)
        r = subprocess.run(
            [str(ctl_path), "--socket", str(ctl_socket), "status"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
        )
    assert r.returncode == 0
    assert b"from-real" in r.stdout
    assert seen["op"] == "status"
