// SPDX-License-Identifier: Apache-2.0
// Conformance: fork() then exit a child cleanly; parent reads its exit code.
// This is the floor for "libsd does not break fork" — libsd's fork
// handling is the part most likely to break under preload.
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) _exit(42);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) { perror("waitpid"); return 2; }
    if (!WIFEXITED(st)) { fprintf(stderr, "abnormal exit\n"); return 3; }
    if (WEXITSTATUS(st) != 42) { fprintf(stderr, "got %d\n", WEXITSTATUS(st)); return 4; }
    return 0;
}
