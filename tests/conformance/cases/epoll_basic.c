// SPDX-License-Identifier: Apache-2.0
// Conformance: epoll_create1 / epoll_ctl(ADD) / epoll_wait fire on a
// readable socket. Uses socketpair to avoid the listen/connect dance.
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { perror("epoll_create1"); return 2; }
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = sv[0];
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) < 0) { perror("epoll_ctl"); return 3; }

    char b = 'X';
    if (write(sv[1], &b, 1) != 1) { perror("write"); return 4; }
    struct epoll_event got;
    int n = epoll_wait(ep, &got, 1, 1000);
    if (n != 1) { perror("epoll_wait"); return 5; }
    if (got.data.fd != sv[0]) return 6;
    if (!(got.events & EPOLLIN)) return 7;

    close(sv[0]); close(sv[1]); close(ep);
    return 0;
}
