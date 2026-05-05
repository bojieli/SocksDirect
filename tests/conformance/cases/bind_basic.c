// SPDX-License-Identifier: Apache-2.0
// Conformance: bind to 127.0.0.1:0, getsockname recovers a non-zero port.
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
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 2; }
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr*)&a, &l) < 0) { perror("getsockname"); return 3; }
    if (ntohs(a.sin_port) == 0) { fprintf(stderr, "port still 0\n"); return 4; }
    close(fd);
    return 0;
}
