// SPDX-License-Identifier: Apache-2.0
// Conformance: getsockname / getpeername return what bind/connect set.
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
    if (listen(srv, 4) < 0) { perror("listen"); return 4; }

    pid_t pid = fork();
    if (pid < 0) return 5;
    if (pid == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr*)&a, sizeof(a)) < 0) _exit(10);
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        if (getpeername(c, (struct sockaddr*)&pa, &pl) < 0) _exit(11);
        if (pa.sin_port != a.sin_port) _exit(12);
        if (pa.sin_addr.s_addr != a.sin_addr.s_addr) _exit(13);
        close(c);
        _exit(0);
    }
    int s = accept(srv, NULL, NULL);
    if (s < 0) return 20;
    close(s);
    close(srv);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 30;
}
