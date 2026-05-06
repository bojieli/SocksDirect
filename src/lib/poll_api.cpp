// SPDX-License-Identifier: Apache-2.0
//
// libsd epoll family hooks.
//
// EPOLLHUP / EPOLLRDHUP were UNSUPPORTED in the prototype. Phase 3 fix
// here is structural: we let glibc deliver them and pass the bits
// through unchanged. (The real fix lives in the SHM data plane in a
// future PR; today we don't drop them.)

#include "src/lib/intercept.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"

#include <sys/epoll.h>

namespace sdp = socksdirect::preload;

DECLARE_REAL(int, epoll_create,  int)
DECLARE_REAL(int, epoll_create1, int)
DECLARE_REAL(int, epoll_ctl,     int, int, int, struct epoll_event*)
DECLARE_REAL(int, epoll_wait,    int, struct epoll_event*, int, int)
DECLARE_REAL(int, epoll_pwait,   int, struct epoll_event*, int, int,
                                 const sigset_t*)

SOCKSDIRECT_HOOK
int epoll_create(int size) {
    return REAL(epoll_create)(size);
}

SOCKSDIRECT_HOOK
int epoll_create1(int flags) {
    return REAL(epoll_create1)(flags);
}

SOCKSDIRECT_HOOK
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
    return REAL(epoll_ctl)(epfd, op, fd, ev);
}

SOCKSDIRECT_HOOK
int epoll_wait(int epfd, struct epoll_event* events, int max, int timeout) {
    return REAL(epoll_wait)(epfd, events, max, timeout);
}

SOCKSDIRECT_HOOK
int epoll_pwait(int epfd, struct epoll_event* events, int max, int timeout,
                const sigset_t* mask) {
    return REAL(epoll_pwait)(epfd, events, max, timeout, mask);
}
