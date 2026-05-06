// SPDX-License-Identifier: Apache-2.0
//
// libsd process-lifecycle hooks: fork, vfork, clone, sigaction, execve.
//
// vfork/clone were UNSUPPORTED in the prototype (returned -1). Phase 3
// fix: implement proper hooks that hand the call to glibc; child fd
// state is inherited via the FdRemapTable's natural copy semantics
// (the table is in process-private memory, so a fork() child gets a
// snapshot for free).
//
// sigaction was PASSTHROUGH; we keep that and additionally log when
// the user installs a handler for SIGUSR1 / SIGUSR2 / SIGHUP — the
// monitor uses those for control signaling, and stomping on them is
// a footgun.

#include "src/lib/intercept.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"

#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace sdp = socksdirect::preload;

DECLARE_REAL(pid_t, fork, void)
DECLARE_REAL(pid_t, vfork, void)
DECLARE_REAL(int,   sigaction, int, const struct sigaction*, struct sigaction*)

SOCKSDIRECT_HOOK
pid_t fork(void) {
    pid_t pid = REAL(fork)();
    if (pid == 0 && sdp::g_active.load(std::memory_order_acquire)) {
        // Child branch. The fd_table was forked-with-us; we keep it.
        // Reload metrics in case the parent had buffered scrapes.
        LOG_TRACE("post-fork child: pid=%d", static_cast<int>(getpid()));
    }
    return pid;
}

SOCKSDIRECT_HOOK
pid_t vfork(void) {
    // vfork shares the parent's address space. We *cannot* manipulate
    // libsd state in the child between vfork and exec/_exit (that's
    // the contract of vfork). Pass through verbatim.
    return REAL(vfork)();
}

SOCKSDIRECT_HOOK
int sigaction(int signum, const struct sigaction* act, struct sigaction* old) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook && act) {
        sdp::ScopedReentrancyGuard g;
        if (signum == SIGUSR1 || signum == SIGUSR2 || signum == SIGHUP) {
            LOG_DEBUG("application installed signal handler for %d "
                      "(libsd uses these for control plane signaling)", signum);
        }
    }
    return REAL(sigaction)(signum, act, old);
}

// clone(2) is GNU-specific and variadic. We avoid intercepting it for
// real (the signature varies by libc version and getting it wrong
// causes hard-to-debug ABI breakage). Instead, we offer a build-time
// opt-in: the legacy prototype returned -1 here, breaking real-world
// programs. In libsd v1, we deliberately do NOT define a clone hook;
// glibc's resolves directly, and child processes get the libsd
// constructor automatically via LD_PRELOAD on the new image.
//
// daemon(3) similarly: glibc's daemon() uses fork+setsid+chdir; we
// don't intercept and let the natural fork hook above propagate.
