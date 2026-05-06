"""End-to-end test: per-pid SHM metrics dump + monitor aggregation.

The libsd-side metrics dumper writes a Prometheus-text snapshot to
$SOCKSDIRECT_LIB_METRICS_DIR/<pid>.prom every second + on close. The
monitor's `lib-metrics` ctl op reads + aggregates the directory.

Validates: counters > 0 after a real workload; bytes_sent equals the
expected payload; stale files (dead pid) are scrubbed by the monitor.
"""
from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

REPO    = Path(__file__).resolve().parents[2]
LIB     = REPO / "build" / "libsd.so"
MON     = REPO / "build" / "socksdirect-monitor"
CTL     = REPO / "build" / "socksdirect-ctl"
SERVER  = REPO / "build" / "apps" / "rpclib-demo" / "rpclib_server"
CLIENT  = REPO / "build" / "apps" / "rpclib-demo" / "rpclib_client"


def _need(*ps):
    for p in ps:
        if not Path(p).exists():
            pytest.skip(f"missing: {p}")


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def _start_monitor(sock_path, mdir):
    env = os.environ.copy()
    env["SOCKSDIRECT_MONITOR_LIB_METRICS_DIR"] = str(mdir)
    p = subprocess.Popen(
        [str(MON), "--control-socket", str(sock_path), "--log-level", "warn"],
        env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 5
    while time.time() < deadline and not sock_path.exists():
        time.sleep(0.02)
    return p


def _ctl(ctl_sock, *args, timeout=5):
    env = os.environ.copy()
    env["SOCKSDIRECT_CTL_SOCKET"] = str(ctl_sock)
    return subprocess.run(
        [str(CTL), *args], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
    )


def _scrape_lib_metrics(ctl_sock):
    """Parse the lib-metrics output into {(name, pid_label): float}."""
    r = _ctl(ctl_sock, "lib-metrics")
    assert r.returncode == 0, r.stderr
    out = {}
    for line in r.stdout.decode().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        m = re.match(
            r"^([A-Za-z_][A-Za-z_0-9]*)\{pid=\"([^\"]+)\"\}\s+(\S+)$", line)
        if m:
            out[(m.group(1), m.group(2))] = float(m.group(3))
    return out


def test_lib_metrics_export_round_trip(tmp_path):
    _need(LIB, MON, CTL, SERVER, CLIENT)
    sock  = tmp_path / "ctl.sock"
    mdir  = tmp_path / "lib-metrics"
    mdir.mkdir()

    mon = _start_monitor(sock, mdir)
    try:
        port = _free_port()
        env = os.environ.copy()
        env["LD_PRELOAD"]                       = str(LIB)
        env["SOCKSDIRECT_MONITOR_CONTROL_SOCKET"] = str(sock)
        env["SOCKSDIRECT_LIB_METRICS_DIR"]      = str(mdir)
        env["SOCKSDIRECT_LOG"]                  = "warn"

        srv = subprocess.Popen([str(SERVER), str(port)], env=env,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        time.sleep(0.3)
        cli = subprocess.run(
            [str(CLIENT), "10000", str(port)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=15,
        )
        assert cli.returncode == 0, cli.stderr
        # Wait for the dumper thread (1s interval).
        time.sleep(1.3)

        metrics = _scrape_lib_metrics(sock)
        # We expect at least the client's pid in the labels.
        sent_keys = [k for k in metrics
                     if k[0] == "socksdirect_lib_shm_bytes_sent_total"]
        assert sent_keys, metrics

        total_sent = sum(metrics[k] for k in sent_keys)
        # 10000 round-trips * 64 B per direction. Each side records
        # its own bytes_sent (= 640 000), so the sum across both
        # peers' .prom files should be ≥ 1.28 MB. Allow some slop in
        # case one side's final dump hasn't landed yet.
        assert total_sent >= 1_200_000, (
            f"only {total_sent} bytes accounted for; expected ~1.28 MB "
            f"(client+server combined). per-pid: {dict(metrics)}")

        # conns_total should be at least 2 (client + server each
        # reported 1 conn).
        conns_keys = [k for k in metrics
                      if k[0] == "socksdirect_lib_shm_conns_total"]
        assert sum(metrics[k] for k in conns_keys) >= 2

        srv.terminate(); srv.wait(timeout=5)
    finally:
        mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)


def test_lib_metrics_scrubs_stale_files(tmp_path):
    _need(LIB, MON, CTL)
    sock = tmp_path / "ctl.sock"
    mdir = tmp_path / "lib-metrics"
    mdir.mkdir()
    # Plant a stale snapshot for a pid that doesn't exist.
    stale = mdir / "999999.prom"
    stale.write_text("# stale metric\nsocksdirect_test{pid=\"999999\"} 0\n")
    assert stale.exists()

    mon = _start_monitor(sock, mdir)
    try:
        r = _ctl(sock, "lib-metrics")
        assert r.returncode == 0, r.stderr
        # The file should be unlinked by the scan; pidof 999999 likely
        # doesn't exist.
        assert not stale.exists(), \
            f"stale file should have been scrubbed; out={r.stdout!r}"
    finally:
        mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)


def test_lib_metrics_op_handles_missing_dir(tmp_path):
    _need(LIB, MON, CTL)
    sock = tmp_path / "ctl.sock"
    bogus = tmp_path / "nope-this-doesnt-exist"
    mon = _start_monitor(sock, bogus)
    try:
        r = _ctl(sock, "lib-metrics")
        assert r.returncode == 0, r.stderr
        assert b"no lib-metrics directory" in r.stdout
    finally:
        mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)
