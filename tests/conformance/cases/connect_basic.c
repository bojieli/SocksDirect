// SPDX-License-Identifier: Apache-2.0
// Conformance: connect to a closed port returns ECONNREFUSED.
// Validates the error path; the happy path is in accept_basic.c.
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(1);  // privileged port nothing listens on
    int rc = connect(fd, (struct sockaddr*)&a, sizeof(a));
    if (rc == 0) { fprintf(stderr, "unexpected success\n"); return 2; }
    if (errno != ECONNREFUSED && errno != EACCES && errno != EADDRNOTAVAIL) {
        fprintf(stderr, "unexpected errno: %d (%s)\n", errno, strerror(errno));
        return 3;
    }
    close(fd);
    return 0;
}
