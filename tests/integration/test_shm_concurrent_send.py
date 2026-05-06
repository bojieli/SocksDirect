"""Multi-thread send() concurrency test.

Two threads in the same process call send() on the same fd. Without
a producer-side mutex, the SPSC ring's head/tail bookkeeping
corrupts under racing producers. With the mutex, all bytes arrive
in *some* order on the receiving side (we don't promise interleaving
order, only that no bytes are lost or duplicated).
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
LIB  = REPO / "build" / "libsd.so"
MON  = REPO / "build" / "socksdirect-monitor"


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


@pytest.fixture
def concurrent_client(tmp_path):
    src = tmp_path / "cc.c"
    src.write_text(r"""
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define N_THREADS 8
#define PER_THREAD 50000
#define MSG 32

static int sock_fd;

static void* sender(void* arg) {
    long tid = (long)arg;
    char buf[MSG];
    memset(buf, (int)tid + 1, MSG);  // distinguishable per-thread byte
    for (int i = 0; i < PER_THREAD; ++i) {
        ssize_t off = 0;
        while (off < MSG) {
            ssize_t r = send(sock_fd, buf + off, MSG - off, 0);
            if (r <= 0) { perror("send"); return (void*)1; }
            off += r;
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(sock_fd, (struct sockaddr*)&a, sizeof(a)) < 0) return 2;

    pthread_t ts[N_THREADS];
    for (long i = 0; i < N_THREADS; ++i)
        pthread_create(&ts[i], NULL, sender, (void*)i);
    for (int i = 0; i < N_THREADS; ++i) {
        void* r;
        pthread_join(ts[i], &r);
        if (r) return 3;
    }
    close(sock_fd);
    return 0;
}
""")
    bin = tmp_path / "cc"
    r = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-pthread",
         "-o", str(bin), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()
    return bin


@pytest.fixture
def counting_server(tmp_path):
    src = tmp_path / "cs.c"
    src.write_text(r"""
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int port = atoi(argv[1]);
    long expected = atol(argv[2]);
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
    if (c < 0) return 4;
    long total = 0;
    long histo[256] = {0};
    char buf[4096];
    for (;;) {
        ssize_t r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
        total += r;
        for (ssize_t i = 0; i < r; ++i) histo[(unsigned char)buf[i]]++;
    }
    if (total != expected) {
        fprintf(stderr, "total=%ld expected=%ld\n", total, expected);
        return 5;
    }
    // Each thread sent PER_THREAD * MSG bytes of its tag value.
    // Verify the histogram per tag is consistent with that.
    long PER_THREAD = 50000;
    long MSG = 32;
    for (int t = 1; t <= 8; ++t) {
        long want = PER_THREAD * MSG;
        if (histo[t] != want) {
            fprintf(stderr, "tag %d: got %ld want %ld\n", t, histo[t], want);
            return 6;
        }
    }
    printf("ok %ld bytes\n", total);
    close(c); close(srv);
    return 0;
}
""")
    bin = tmp_path / "cs"
    r = subprocess.run(
        ["cc", "-O0", "-g", "-D_GNU_SOURCE", "-o", str(bin), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert r.returncode == 0, r.stderr.decode()
    return bin


def test_concurrent_send_no_corruption(tmp_path, concurrent_client, counting_server):
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

    expected = 8 * 50000 * 32  # threads * iters * msg
    srv = subprocess.Popen(
        [str(counting_server), str(port), str(expected)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    # Wait for "ready".
    deadline = time.time() + 5
    while time.time() < deadline:
        line = srv.stdout.readline()
        if b"ready" in line: break

    cli = subprocess.run(
        [str(concurrent_client), str(port)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
    )
    assert cli.returncode == 0, cli.stderr

    rc = srv.wait(timeout=10)
    out = srv.stdout.read().decode()
    err = srv.stderr.read().decode()
    assert rc == 0, f"server failed: {err}\n{out}"
    assert f"ok {expected} bytes" in out, out

    mon.send_signal(signal.SIGTERM); mon.wait(timeout=5)
