// SPDX-License-Identifier: Apache-2.0
// Conformance baseline: clone with CLONE_VM | SIGCHLD.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int child(void* arg) { (void)arg; return 9; }

int main(void) {
    enum { STK = 64 * 1024 };
    char* stack = malloc(STK);
    if (!stack) return 1;
    pid_t pid = clone(child, stack + STK, CLONE_VM | SIGCHLD, NULL);
    if (pid < 0) { perror("clone"); free(stack); return 2; }
    int st = 0;
    waitpid(pid, &st, 0);
    free(stack);
    return WIFEXITED(st) && WEXITSTATUS(st) == 9 ? 0 : 3;
}
