"""Verify the futex-based recv path doesn't burn CPU when idle.

Stand up a server-client pair under preload. Have the client connect
but pause for 1 second before sending anything. The server is in
shm_recv blocked on its inbound ring. Without the futex, that path
spin-yielded indefinitely — visible as CPU time in the server proc.
With the futex, the server should consume <50 ms of CPU time during
the 1s idle window.
"""
from __future__ import annotations

import os
import resource
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

REPO    = Path(__file__).resolve().parents[2]
LIB     = REPO / "build" / "libsd.so"
MON     = REPO / "build" / "socksdirect-monitor"


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


def _read_proc_stat_cpu_jiffies(pid: int) -> int:
    """Sum of utime + stime in jiffies for the given pid."""
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().split()
    # /proc/N/stat: fields 14 (utime) and 15 (stime), 1-indexed; with
    # the comm field potentially containing spaces. Find them by
    # walking past the closing ')' of the comm.
    s = open(f"/proc/{pid}/stat").read()
    rp = s.rindex(")")
    after = s[rp + 1:].split()
    # after[0] = state, after[11] = utime, after[12] = stime
    utime = int(after[11])
    stime = int(after[12])
    return utime + stime


@pytest.fixture
def idle_server_src(tmp_path):
    src = tmp_path / "is.c"
    src.write_text(r"""
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(srv, (struct sockaddr*)&a, sizeof(a)) < 0) return 2;
    if (listen(srv, 4) < 0) return 3;
    printf("ready\n"); fflush(stdout);
    int c = accept(srv, NULL, NULL);
    char buf[64];
    while (1) {
        ssize_t r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
    }
    close(c); close(srv); return 0;
}
""")
    bin = tmp_path / "is"
    r = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(bin), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()
    return bin


def test_idle_recv_does_not_burn_cpu(tmp_path, idle_server_src):
    _need(LIB, MON)
    sock = tmp_path / "ctl.sock"
    mon = subprocess.Popen(
        [str(MON), "--control-socket", str(sock), "--log-level", "warn"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 5
    while time.time() < deadline and not sock.exists():
        time.sleep(0.02)

    port = _free_port()
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(LIB)
    env["SOCKSDIRECT_MONITOR_CONTROL_SOCKET"] = str(sock)
    env["SOCKSDIRECT_LOG"] = "warn"

    srv = subprocess.Popen(
        [str(idle_server_src), str(port)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    deadline = time.time() + 5
    while time.time() < deadline:
        line = srv.stdout.readline()
        if b"ready" in line: break

    # Open a client TCP connection so the server's accept returns.
    # The server then enters recv(), which (under SHM) blocks.
    csock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Preload via env on the client side so it also takes the SHM path.
    # Easiest: spawn a subprocess that just connects, sleeps, then sends.
    (tmp_path / "client.c").write_text(r"""
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    int sleep_ms = atoi(argv[2]);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) return 2;
    usleep(sleep_ms * 1000);
    char b = 'X';
    if (send(fd, &b, 1, 0) != 1) return 3;
    close(fd);
    return 0;
}
""")
    cli_bin = tmp_path / "cl"
    subprocess.run(["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(cli_bin),
                    str(tmp_path / "client.c")], check=True)

    cli = subprocess.Popen(
        [str(cli_bin), str(port), "1000"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    # Wait for the SHM connection to be established.
    time.sleep(0.2)

    j0 = _read_proc_stat_cpu_jiffies(srv.pid)
    # Idle window. The server's recv is blocked; the client is
    # sleeping in usleep. No data is moving.
    time.sleep(0.6)
    j1 = _read_proc_stat_cpu_jiffies(srv.pid)
    cli.wait(timeout=5)
    srv.wait(timeout=5)

    HZ = os.sysconf("SC_CLK_TCK")
    seconds = (j1 - j0) / HZ
    print(f"server consumed {seconds*1000:.1f} ms of CPU during 600 ms idle")
    # Allowable budget: under 100 ms (= 17% of 600 ms wall). Without
    # the futex this was effectively 100% of one core (~600 ms).
    assert seconds < 0.1, f"server burned {seconds:.2f}s of CPU while idle"

    mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)
