// SPDX-License-Identifier: Apache-2.0
//
// libsd socket-family hooks.
//
// All hooks fall through to glibc when the runtime is not yet active
// or when the fd doesn't belong to libsd. Hooks register every TCP
// socket fd with the FdRemapTable so the rest of the API surface can
// recognise it.

#include "src/lib/intercept.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace sd  = socksdirect;
namespace sdp = socksdirect::preload;

DECLARE_REAL(int,    socket,    int, int, int)
DECLARE_REAL(int,    bind,      int, const struct sockaddr*, socklen_t)
DECLARE_REAL(int,    listen,    int, int)
DECLARE_REAL(int,    accept,    int, struct sockaddr*, socklen_t*)
DECLARE_REAL(int,    accept4,   int, struct sockaddr*, socklen_t*, int)
DECLARE_REAL(int,    connect,   int, const struct sockaddr*, socklen_t)
DECLARE_REAL(ssize_t, send,     int, const void*, size_t, int)
DECLARE_REAL(ssize_t, recv,     int, void*, size_t, int)
DECLARE_REAL(ssize_t, sendto,   int, const void*, size_t, int,
                                const struct sockaddr*, socklen_t)
DECLARE_REAL(ssize_t, recvfrom, int, void*, size_t, int,
                                struct sockaddr*, socklen_t*)
DECLARE_REAL(ssize_t, sendmsg,  int, const struct msghdr*, int)
DECLARE_REAL(ssize_t, recvmsg,  int, struct msghdr*, int)
DECLARE_REAL(int,    shutdown,  int, int)
DECLARE_REAL(int,    getsockopt, int, int, int, void*, socklen_t*)
DECLARE_REAL(int,    setsockopt, int, int, int, const void*, socklen_t)
DECLARE_REAL(int,    getsockname, int, struct sockaddr*, socklen_t*)
DECLARE_REAL(int,    getpeername, int, struct sockaddr*, socklen_t*)
DECLARE_REAL(int,    socketpair, int, int, int, int*)

namespace {

// Decide whether libsd should track this socket. Only AF_INET and
// AF_INET6 / SOCK_STREAM enter the table; everything else is just
// counted as passthrough.
sd::FdType classify(int domain, int type) {
    int t = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if ((domain == AF_INET || domain == AF_INET6) && t == SOCK_STREAM) {
        return sd::kSocket;
    }
    return sd::kSystem;
}

}  // namespace

SOCKSDIRECT_HOOK
int socket(int domain, int type, int protocol) {
    int fd = REAL(socket)(domain, type, protocol);
    if (fd < 0) return fd;
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return fd;
    sdp::ScopedReentrancyGuard g;
    sdp::state().m_socket_total->inc();
    sd::FdType cls = classify(domain, type);
    // We currently track but do not accelerate: register the fd so dup
    // and close see consistent state.
    sdp::state().fd_table.alloc(cls, fd);
    LOG_TRACE("socket(%d,%d,%d) -> %d (%s)", domain, type, protocol, fd,
              cls == sd::kSocket ? "tracked" : "passthrough");
    return fd;
}

SOCKSDIRECT_HOOK
int bind(int fd, const struct sockaddr* a, socklen_t l) {
    return REAL(bind)(fd, a, l);
}

SOCKSDIRECT_HOOK
int listen(int fd, int backlog) {
    return REAL(listen)(fd, backlog);
}

SOCKSDIRECT_HOOK
int accept(int fd, struct sockaddr* a, socklen_t* l) {
    int c = REAL(accept)(fd, a, l);
    if (c >= 0 && sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        sdp::state().fd_table.alloc(sd::kSocket, c);
    }
    return c;
}

SOCKSDIRECT_HOOK
int accept4(int fd, struct sockaddr* a, socklen_t* l, int flags) {
    int c = REAL(accept4)(fd, a, l, flags);
    if (c >= 0 && sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        sdp::state().fd_table.alloc(sd::kSocket, c);
    }
    return c;
}

SOCKSDIRECT_HOOK
int connect(int fd, const struct sockaddr* a, socklen_t l) {
    return REAL(connect)(fd, a, l);
}

SOCKSDIRECT_HOOK
ssize_t send(int fd, const void* buf, size_t n, int flags) {
    return REAL(send)(fd, buf, n, flags);
}

SOCKSDIRECT_HOOK
ssize_t recv(int fd, void* buf, size_t n, int flags) {
    return REAL(recv)(fd, buf, n, flags);
}

SOCKSDIRECT_HOOK
ssize_t sendto(int fd, const void* buf, size_t n, int flags,
               const struct sockaddr* a, socklen_t l) {
    return REAL(sendto)(fd, buf, n, flags, a, l);
}

SOCKSDIRECT_HOOK
ssize_t recvfrom(int fd, void* buf, size_t n, int flags,
                 struct sockaddr* a, socklen_t* l) {
    return REAL(recvfrom)(fd, buf, n, flags, a, l);
}

SOCKSDIRECT_HOOK
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    // PARTIAL per docs/API.md: ancillary data passes through;
    // payload uses fast path. Today both go to glibc.
    return REAL(sendmsg)(fd, msg, flags);
}

SOCKSDIRECT_HOOK
ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    return REAL(recvmsg)(fd, msg, flags);
}

// shutdown was UNSUPPORTED in the prototype (silently ignored).
// Phase 3 fix: call through to glibc with proper error propagation
// AND log the half-close so SHM peers (Phase 4+) can react.
SOCKSDIRECT_HOOK
int shutdown(int fd, int how) {
    int rc = REAL(shutdown)(fd, how);
    if (rc == 0 && sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        LOG_TRACE("shutdown(%d, %d) ok", fd, how);
    }
    return rc;
}

SOCKSDIRECT_HOOK
int getsockopt(int fd, int level, int opt, void* val, socklen_t* l) {
    return REAL(getsockopt)(fd, level, opt, val, l);
}

SOCKSDIRECT_HOOK
int setsockopt(int fd, int level, int opt, const void* val, socklen_t l) {
    return REAL(setsockopt)(fd, level, opt, val, l);
}

SOCKSDIRECT_HOOK
int getsockname(int fd, struct sockaddr* a, socklen_t* l) {
    return REAL(getsockname)(fd, a, l);
}

SOCKSDIRECT_HOOK
int getpeername(int fd, struct sockaddr* a, socklen_t* l) {
    return REAL(getpeername)(fd, a, l);
}

SOCKSDIRECT_HOOK
int socketpair(int domain, int type, int proto, int sv[2]) {
    return REAL(socketpair)(domain, type, proto, sv);
}
