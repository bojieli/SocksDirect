// SPDX-License-Identifier: Apache-2.0
// Conformance: close on a valid socket succeeds; close on an already-
// closed fd returns -1 with errno=EBADF.
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    if (close(fd) < 0) { perror("close"); return 2; }
    if (close(fd) == 0) { fprintf(stderr, "double-close succeeded\n"); return 3; }
    if (errno != EBADF) { fprintf(stderr, "errno %d\n", errno); return 4; }
    return 0;
}
