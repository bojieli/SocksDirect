// SPDX-License-Identifier: Apache-2.0
// Conformance: listen on 127.0.0.1:0; child connects; parent accepts;
// each side reads/writes one byte. Validates the full intra-host TCP
// life cycle through libc (or libsd, if preloaded).
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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
        if (c < 0) { perror("c-socket"); _exit(10); }
        if (connect(c, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("connect"); _exit(11); }
        char b = 'X';
        if (send(c, &b, 1, 0) != 1) { perror("send"); _exit(12); }
        if (recv(c, &b, 1, 0) != 1) { perror("recv"); _exit(13); }
        if (b != 'Y') _exit(14);
        close(c);
        _exit(0);
    }
    int s = accept(srv, NULL, NULL);
    if (s < 0) { perror("accept"); return 20; }
    char b = 0;
    if (recv(s, &b, 1, 0) != 1) { perror("recv-srv"); return 21; }
    if (b != 'X') return 22;
    b = 'Y';
    if (send(s, &b, 1, 0) != 1) { perror("send-srv"); return 23; }
    close(s);
    close(srv);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) { perror("waitpid"); return 24; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "child status: %d\n", st); return 25;
    }
    return 0;
}
