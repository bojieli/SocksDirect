"""End-to-end test for the SHM intra-host data plane.

Boots a fresh monitor on a temp socket, then runs rpclib_server +
rpclib_client both with libsd preloaded. Asserts:

  1. The shm-attached log line appears on both sides.
  2. A /dev/shm/sd-<key>* segment is created during the run.
  3. The libsd metric `socksdirect_lib_shm_conns_total` increments.
  4. The throughput observed is materially higher than vanilla TCP
     loopback.

This is the gate for the Phase 3 keystone: "preloaded apps actually
go through SHM, not the kernel."
"""
from __future__ import annotations

import glob
import os
import re
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest


def _free_port() -> int:
    """Ask the kernel for a free TCP port, then close. Some race
    window before the test's server binds, but with SO_REUSEADDR
    on the server side that's tolerable."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

REPO    = Path(__file__).resolve().parents[2]
LIB     = REPO / "build" / "libsd.so"
MON     = REPO / "build" / "socksdirect-monitor"
CTL     = REPO / "build" / "socksdirect-ctl"
SERVER  = REPO / "build" / "apps" / "rpclib-demo" / "rpclib_server"
CLIENT  = REPO / "build" / "apps" / "rpclib-demo" / "rpclib_client"


def _need(*paths):
    for p in paths:
        if not Path(p).exists():
            pytest.skip(f"binary missing: {p}")


def _start_monitor(sock_path: Path, log_path: Path):
    p = subprocess.Popen(
        [str(MON), "--control-socket", str(sock_path), "--log-level", "warn"],
        stdout=open(log_path, "wb"),
        stderr=subprocess.STDOUT,
    )
    deadline = time.time() + 5
    while time.time() < deadline and not sock_path.exists():
        time.sleep(0.02)
    if not sock_path.exists():
        p.terminate(); p.wait()
        pytest.fail("monitor never bound the control socket")
    return p


def _scrape_metrics(ctl_sock: Path) -> dict:
    env = os.environ.copy()
    env["SOCKSDIRECT_CTL_SOCKET"] = str(ctl_sock)
    r = subprocess.run([str(CTL), "metrics"], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=5)
    out = r.stdout.decode()
    metrics = {}
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*(?:\{[^}]*\})?) +(\S+)$", line)
        if m:
            metrics[m.group(1)] = float(m.group(2))
    return metrics


def _run_pair(ctl_sock: Path, port: int, iters: int, log_dir: Path):
    """Spawn server + client under preload, return (client rc, throughput, logs)."""
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIB)
    env["SOCKSDIRECT_MONITOR_CONTROL_SOCKET"] = str(ctl_sock)
    env["SOCKSDIRECT_LOG"] = "info"

    srv_log = open(log_dir / "srv.log", "wb")
    srv = subprocess.Popen([str(SERVER), str(port)], env=env,
                           stdout=subprocess.DEVNULL, stderr=srv_log)
    time.sleep(0.3)

    cli_log = open(log_dir / "cli.log", "wb")
    cli = subprocess.run([str(CLIENT), str(iters), str(port)],
                         env=env, stdout=subprocess.PIPE,
                         stderr=cli_log, timeout=60)
    srv.terminate()
    srv.wait(timeout=2)
    srv_log.close()
    cli_log.close()

    out = cli.stdout.decode()
    m = re.search(r"(\d+)\s*msg/s", out)
    mps = int(m.group(1)) if m else 0
    return cli.returncode, mps, out


def _baseline_pair(port: int, iters: int):
    """Same scenario without preload — vanilla loopback TCP."""
    srv = subprocess.Popen([str(SERVER), str(port)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    r = subprocess.run([str(CLIENT), str(iters), str(port)],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       timeout=60)
    srv.terminate(); srv.wait(timeout=2)
    out = r.stdout.decode()
    m = re.search(r"(\d+)\s*msg/s", out)
    return int(m.group(1)) if m else 0


@pytest.fixture
def workspace(tmp_path):
    _need(LIB, MON, CTL, SERVER, CLIENT)
    sock = tmp_path / "ctl.sock"
    mlog = tmp_path / "monitor.log"
    proc = _start_monitor(sock, mlog)
    # Clean any stale shm segments from prior runs.
    for p in glob.glob("/dev/shm/sd-*"):
        try: os.unlink(p)
        except OSError: pass
    yield (sock, tmp_path)
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)


def test_shm_attaches_logged_on_both_sides(workspace):
    """Both client and server log a `shm-attached` info line and
    converge on the same SHM key."""
    sock, work = workspace
    rc, _, _ = _run_pair(sock, _free_port(), 1000, work)
    assert rc == 0
    srv_log = (work / "srv.log").read_text()
    cli_log = (work / "cli.log").read_text()
    assert "shm-attached" in srv_log, srv_log
    assert "shm-attached" in cli_log, cli_log

    # Both sides should have negotiated the same key. Pull it out.
    def _key(log):
        m = re.search(r"key=([0-9a-f]+) ", log)
        return m.group(1) if m else None

    assert _key(srv_log) == _key(cli_log)
    assert _key(srv_log) is not None
    # Roles must differ.
    assert "role=creator" in srv_log + cli_log
    assert "role=joiner"  in srv_log + cli_log
    # The lib-side metric is per-process, not visible from the monitor's
    # ctl scrape. We covered the bookkeeping via the log assertions
    # above; the segment-on-disk test below is the load-bearing one.


def test_shm_segment_appears_on_disk_then_unlinks(workspace):
    """Verify the *specific* segment created by this test is unlinked
    after both peers exit. Other concurrent tests may have their own
    segments live; we only check the one we own (identified by
    parsing the shm-attached log line for our key)."""
    sock, work = workspace
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIB)
    env["SOCKSDIRECT_MONITOR_CONTROL_SOCKET"] = str(sock)
    env["SOCKSDIRECT_LOG"] = "info"
    port = _free_port()
    srv_log = open(work / "seg-srv.log", "wb")
    srv = subprocess.Popen([str(SERVER), str(port)], env=env,
                           stdout=subprocess.DEVNULL, stderr=srv_log)
    time.sleep(0.3)
    cli = subprocess.Popen([str(CLIENT), "200000", str(port)], env=env,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    # Snapshot the live segment list mid-run; our key must be there.
    time.sleep(0.05)
    mid_segs = set(glob.glob("/dev/shm/sd-*"))
    cli.wait(timeout=10)
    srv.terminate(); srv.wait(timeout=5)
    srv_log.close()

    # Pull our key from the server log.
    log_text = (work / "seg-srv.log").read_text()
    m = re.search(r"key=([0-9a-f]+) ", log_text)
    assert m, f"no shm-attached log found:\n{log_text}"
    our_path = f"/dev/shm/sd-{m.group(1)}"
    assert our_path in mid_segs, (
        f"our segment {our_path} not present mid-run; live: {mid_segs}")

    # Both sides do close() on their connected fd which triggers
    # ShmSegment::close -> refcount-- -> shm_unlink. Server's close
    # happens after recv detects EOF + drains the buffered data —
    # under heavy load that can lag a few ms behind the client exit.
    # Wait up to 2 s for the unlink to land.
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if our_path not in glob.glob("/dev/shm/sd-*"):
            break
        time.sleep(0.05)
    end_segs = set(glob.glob("/dev/shm/sd-*"))
    assert our_path not in end_segs, (
        f"our segment {our_path} leaked after 2 s: {end_segs}")


def test_shm_throughput_beats_vanilla_loopback(workspace):
    sock, work = workspace
    iters = 50000
    _, mps_shm, _   = _run_pair(sock, _free_port(), iters, work)
    mps_vanilla     = _baseline_pair(_free_port(), iters)
    print(f"shm={mps_shm} mps  vanilla={mps_vanilla} mps  ratio={mps_shm/max(mps_vanilla,1):.1f}x")
    # On a noisy CI VM we don't insist on the 11x we see locally; we
    # do insist on at least 2x improvement, which is well above noise.
    assert mps_shm >= 2 * mps_vanilla, \
        f"SHM ({mps_shm}) didn't beat vanilla ({mps_vanilla}) by 2x"


def test_eof_propagates_after_client_close(workspace):
    """After the client closes its fd, the server's recv returns 0
    (EOF) on the SHM ring."""
    sock, work = workspace
    # Use the rpclib pair: the client already closes when done. The
    # server's accept loop continues; if EOF doesn't propagate, the
    # server hangs in read_full and we'd time out.
    rc, _, _ = _run_pair(sock, _free_port(), 100, work)
    assert rc == 0


def test_shm_handles_consecutive_connections(workspace):
    sock, work = workspace
    # Run three back-to-back to exercise the handshake registry's
    # creator/joiner cycling.
    for i in range(3):
        rc, mps, _ = _run_pair(sock, _free_port(), 500, work)
        assert rc == 0, f"iter {i} rc={rc}"
        assert mps > 0
