// SPDX-License-Identifier: Apache-2.0
// Conformance: dup/dup2/dup3 on a regular fd (not a socket) MUST work
// — libsd is supposed to passthrough non-socket fds. The unsupported
// path is socket fds, which we test under preload separately.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    int d = dup(fd);
    if (d < 0) { perror("dup"); return 2; }
    int d2 = dup2(fd, 100);
    if (d2 != 100) { perror("dup2"); return 3; }
    int d3 = dup3(fd, 101, O_CLOEXEC);
    if (d3 != 101) { perror("dup3"); return 4; }
    close(fd); close(d); close(d2); close(d3);
    return 0;
}
