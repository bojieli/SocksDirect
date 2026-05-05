// SPDX-License-Identifier: Apache-2.0
// Conformance baseline (no preload): shutdown(SHUT_WR) makes the peer
// see EOF on read. libsd silently ignores shutdown today; the
// preloaded run of this case is expected to fail until Phase 3.
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    if (shutdown(sv[0], SHUT_WR) < 0) { perror("shutdown"); return 2; }
    char buf[16];
    ssize_t n = read(sv[1], buf, sizeof(buf));
    if (n != 0) { fprintf(stderr, "expected EOF, got %zd\n", n); return 3; }
    close(sv[0]); close(sv[1]);
    return 0;
}
