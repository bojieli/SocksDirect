// SPDX-License-Identifier: Apache-2.0
// Conformance: F_GETFL / F_SETFL round-trip on a socket. Sets O_NONBLOCK,
// reads it back.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) { perror("F_GETFL"); return 2; }
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { perror("F_SETFL"); return 3; }
    int fl2 = fcntl(fd, F_GETFL, 0);
    if (fl2 < 0) { perror("F_GETFL2"); return 4; }
    if (!(fl2 & O_NONBLOCK)) { fprintf(stderr, "O_NONBLOCK not retained\n"); return 5; }
    close(fd);
    return 0;
}
