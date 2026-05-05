// SPDX-License-Identifier: Apache-2.0
// Conformance baseline: vfork+_exit works in glibc. Under libsd this
// is currently unsupported (the child is supposed to share the parent
// VM until exec/_exit; libsd's fork interception doesn't handle that).
// This case validates the libc baseline so the preloaded variance is
// directly attributable to libsd.
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    pid_t pid = vfork();
    if (pid < 0) { perror("vfork"); return 1; }
    if (pid == 0) _exit(7);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 7 ? 0 : 2;
}
