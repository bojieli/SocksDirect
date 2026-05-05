// SPDX-License-Identifier: Apache-2.0
//
// rpclib-demo server — accept loop, echoes 64-byte messages.
//
// Wire format: each message is a fixed 64 bytes, no framing.
// Server reads exactly 64 bytes, writes them back.
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MSG 64

static int read_full(int fd, void* buf, size_t n) {
    char* p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}
static int write_full(int fd, const void* buf, size_t n) {
    const char* p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

int main(int argc, char** argv) {
    int port = argc >= 2 ? atoi(argv[1]) : 57777;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 2; }
    if (listen(srv, 16) < 0) { perror("listen"); return 3; }
    printf("rpclib_server listening on 127.0.0.1:%d\n", port);
    fflush(stdout);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { perror("accept"); continue; }
        char buf[MSG];
        for (;;) {
            if (read_full(c, buf, MSG) < 0) break;
            if (write_full(c, buf, MSG) < 0) break;
        }
        close(c);
    }
}
