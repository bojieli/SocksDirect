"""Integration test: epoll integration with SHM rings.

Two preloaded processes:
  1. Server uses epoll_wait to detect when the client has sent data.
  2. Server's epoll_wait must return readable when the SHM ring fills.
  3. After client closes, server's epoll_wait sees readable+EOF on
     the next recv.
"""
from __future__ import annotations

import os
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


@pytest.fixture
def epoll_server_src(tmp_path):
    src = tmp_path / "es.c"
    src.write_text(r"""
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
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
    printf("listening %d\n", port); fflush(stdout);

    int c = accept(srv, NULL, NULL);
    if (c < 0) return 4;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) return 5;
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = c;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev) < 0) return 6;

    int total = 0;
    char buf[1024];
    for (;;) {
        struct epoll_event got;
        int n = epoll_wait(ep, &got, 1, 5000);
        if (n <= 0) {
            fprintf(stderr, "epoll_wait timeout/err: %d\n", n);
            return 7;
        }
        if (got.data.fd != c) {
            fprintf(stderr, "wrong fd %d (want %d)\n", got.data.fd, c);
            return 8;
        }
        ssize_t r = recv(c, buf, sizeof(buf), 0);
        if (r < 0) return 9;
        if (r == 0) break;  // EOF
        total += (int)r;
    }
    printf("got %d bytes\n", total);
    close(c); close(ep); close(srv);
    return 0;
}
""")
    bin = tmp_path / "es"
    r = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(bin), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()
    return bin


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def test_epoll_fires_on_shm_ring(tmp_path, epoll_server_src):
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
        [str(epoll_server_src), str(port)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    # Wait for the server's "listening" line.
    deadline = time.time() + 5
    while time.time() < deadline:
        if srv.stdout.peek(1):
            line = srv.stdout.readline()
            if b"listening" in line:
                break
        time.sleep(0.02)

    client_src = tmp_path / "cl.c"
    client_src.write_text(r"""
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    int n = atoi(argv[2]);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) return 2;
    char* buf = (char*)malloc(n);
    memset(buf, 'a', n);
    if (send(fd, buf, n, 0) != n) return 3;
    free(buf);
    close(fd);
    return 0;
}
""")
    client_bin = tmp_path / "cl"
    r = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(client_bin), str(client_src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()

    cli = subprocess.run(
        [str(client_bin), str(port), "8192"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
    )
    assert cli.returncode == 0, cli.stderr

    rc = srv.wait(timeout=10)
    assert rc == 0, srv.stderr.read()
    out = srv.stdout.read().decode()
    # epoll fired and recv saw all 8192 bytes.
    assert "got 8192 bytes" in out, out

    mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)
