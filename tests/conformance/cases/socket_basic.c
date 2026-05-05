// SPDX-License-Identifier: Apache-2.0
// Conformance: socket(AF_INET, SOCK_STREAM, 0) succeeds, returns >= 0,
// can be closed.
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    if (close(fd) < 0) { perror("close"); return 2; }
    return 0;
}
