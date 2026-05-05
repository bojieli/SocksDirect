// SPDX-License-Identifier: Apache-2.0
// Conformance: send/recv exchange a 4-KiB payload intra-host. Also
// covers sendmsg/recvmsg scalar path (no ancillary data).
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define N 4096

static int do_server(struct sockaddr_in* a, int listen_fd) {
    int s = accept(listen_fd, NULL, NULL);
    if (s < 0) { perror("accept"); return 20; }
    char buf[N];
    size_t got = 0;
    while (got < N) {
        ssize_t r = recv(s, buf + got, N - got, 0);
        if (r <= 0) { perror("recv"); return 21; }
        got += (size_t)r;
    }
    // Echo via sendmsg this time.
    struct iovec iov = { buf, N };
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (sendmsg(s, &m, 0) != N) { perror("sendmsg"); return 22; }
    close(s);
    return 0;
    (void)a;
}

int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 2; }
    socklen_t l = sizeof(a);
    if (getsockname(srv, (struct sockaddr*)&a, &l) < 0) { perror("getsockname"); return 3; }
    if (listen(srv, 8) < 0) { perror("listen"); return 4; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 5; }
    if (pid == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (c < 0) _exit(10);
        if (connect(c, (struct sockaddr*)&a, sizeof(a)) < 0) _exit(11);
        char buf[N];
        for (int i = 0; i < N; ++i) buf[i] = (char)(i & 0xff);
        size_t sent = 0;
        while (sent < N) {
            ssize_t s2 = send(c, buf + sent, N - sent, 0);
            if (s2 <= 0) _exit(12);
            sent += (size_t)s2;
        }
        char rbuf[N];
        size_t got = 0;
        // Use recvmsg here to cover that path.
        while (got < N) {
            struct iovec iov = { rbuf + got, N - got };
            struct msghdr m;
            memset(&m, 0, sizeof(m));
            m.msg_iov = &iov;
            m.msg_iovlen = 1;
            ssize_t r = recvmsg(c, &m, 0);
            if (r <= 0) _exit(13);
            got += (size_t)r;
        }
        for (int i = 0; i < N; ++i) if (rbuf[i] != (char)(i & 0xff)) _exit(14);
        close(c);
        _exit(0);
    }
    int rc = do_server(&a, srv);
    close(srv);
    int st = 0;
    waitpid(pid, &st, 0);
    if (rc) return rc;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "child status: %d\n", st); return 30;
    }
    return 0;
}
