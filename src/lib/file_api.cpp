// SPDX-License-Identifier: Apache-2.0
//
// libsd close/dup/dup2/dup3/fcntl hooks.
//
// dup/dup2/dup3 were UNSUPPORTED in the prototype (returned -1 + ERROR
// log). Phase 3 fix: implement them properly using FdRemapTable's
// refcount machinery. close() decrements; the libc close happens when
// the underlying refcount hits 0.

#include "src/lib/intercept.hpp"
#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

namespace sd  = socksdirect;
namespace sdp = socksdirect::preload;

DECLARE_REAL(int, close, int)
DECLARE_REAL(int, dup,   int)
DECLARE_REAL(int, dup2,  int, int)
DECLARE_REAL(int, dup3,  int, int, int)
DECLARE_REAL(int, fcntl, int, int, ...)

SOCKSDIRECT_HOOK
int close(int fd) {
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return REAL(close)(fd);

    sdp::ScopedReentrancyGuard g;
    auto& tbl = sdp::state().fd_table;
    int v = tbl.reverse_lookup(sd::kSocket, fd);
    if (v < 0) v = tbl.reverse_lookup(sd::kSystem, fd);
    if (v < 0) {
        // Not tracked — straight passthrough.
        return REAL(close)(fd);
    }
    int refs = tbl.free(v);
    sdp::state().m_close_total->inc();
    if (refs > 0) {
        // Other vfds still ref this real_fd; don't close.
        return 0;
    }
    // Drop any SHM connection tied to this fd. The ShmConn destructor
    // closes the segment, which marks the outbound ring closed and
    // decrements the segment refcount.
    auto shm = sdp::conn_registry().remove(fd);
    (void)shm;  // RAII: shared_ptr drop closes the segment.
    return REAL(close)(fd);
}

SOCKSDIRECT_HOOK
int dup(int fd) {
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return REAL(dup)(fd);
    sdp::ScopedReentrancyGuard g;
    auto& tbl = sdp::state().fd_table;
    int v = tbl.reverse_lookup(sd::kSocket, fd);
    if (v < 0) v = tbl.reverse_lookup(sd::kSystem, fd);
    if (v < 0) {
        // Not tracked: plain dup.
        return REAL(dup)(fd);
    }
    int new_fd = REAL(dup)(fd);
    if (new_fd < 0) return new_fd;
    auto m = tbl.lookup(v);
    tbl.alloc(m.type, new_fd);
    sdp::state().m_dup_total->inc();
    LOG_TRACE("dup(%d) -> %d", fd, new_fd);
    return new_fd;
}

SOCKSDIRECT_HOOK
int dup2(int oldfd, int newfd) {
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return REAL(dup2)(oldfd, newfd);
    sdp::ScopedReentrancyGuard g;
    auto& tbl = sdp::state().fd_table;
    int v_old = tbl.reverse_lookup(sd::kSocket, oldfd);
    if (v_old < 0) v_old = tbl.reverse_lookup(sd::kSystem, oldfd);

    int rc = REAL(dup2)(oldfd, newfd);
    if (rc < 0) return rc;

    // dup2 of an untracked fd onto an untracked target: nothing to do.
    if (v_old < 0) return rc;

    // Drop newfd's tracking (if any), then alloc fresh against oldfd's class.
    int v_new = tbl.reverse_lookup(sd::kSocket, newfd);
    if (v_new < 0) v_new = tbl.reverse_lookup(sd::kSystem, newfd);
    if (v_new >= 0) tbl.free(v_new);
    auto m = tbl.lookup(v_old);
    tbl.alloc(m.type, newfd);
    sdp::state().m_dup_total->inc();
    LOG_TRACE("dup2(%d,%d) ok", oldfd, newfd);
    return rc;
}

SOCKSDIRECT_HOOK
int dup3(int oldfd, int newfd, int flags) {
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return REAL(dup3)(oldfd, newfd, flags);
    sdp::ScopedReentrancyGuard g;
    auto& tbl = sdp::state().fd_table;
    int v_old = tbl.reverse_lookup(sd::kSocket, oldfd);
    if (v_old < 0) v_old = tbl.reverse_lookup(sd::kSystem, oldfd);

    int rc = REAL(dup3)(oldfd, newfd, flags);
    if (rc < 0) return rc;
    if (v_old < 0) return rc;
    int v_new = tbl.reverse_lookup(sd::kSocket, newfd);
    if (v_new < 0) v_new = tbl.reverse_lookup(sd::kSystem, newfd);
    if (v_new >= 0) tbl.free(v_new);
    auto m = tbl.lookup(v_old);
    tbl.alloc(m.type, newfd);
    sdp::state().m_dup_total->inc();
    return rc;
}

SOCKSDIRECT_HOOK
int fcntl(int fd, int cmd, ...) {
    // fcntl is variadic with cmd-dependent signature. Forward verbatim.
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    int rc = REAL(fcntl)(fd, cmd, arg);
    if (rc >= 0 && sdp::g_active.load(std::memory_order_acquire)
        && !sdp::g_in_hook && cmd == F_DUPFD) {
        sdp::ScopedReentrancyGuard g;
        auto& tbl = sdp::state().fd_table;
        int v = tbl.reverse_lookup(sd::kSocket, fd);
        if (v < 0) v = tbl.reverse_lookup(sd::kSystem, fd);
        if (v >= 0) {
            auto m = tbl.lookup(v);
            tbl.alloc(m.type, rc);
            sdp::state().m_dup_total->inc();
        }
    }
    return rc;
}
