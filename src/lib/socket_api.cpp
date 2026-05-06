// SPDX-License-Identifier: Apache-2.0
//
// libsd socket-family hooks.
//
// All hooks fall through to glibc when the runtime is not yet active
// or when the fd doesn't belong to libsd. Hooks register every TCP
// socket fd with the FdRemapTable so the rest of the API surface can
// recognise it.

#include "src/lib/intercept.hpp"
#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"
#include "socksdirect/metrics.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
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

namespace {

// On accept/connect of a TCP socket we may want to upgrade it to the
// SHM fast path. This helper does the work; it's a no-op if the
// runtime isn't active, the fd isn't AF_INET/SOCK_STREAM, or the
// monitor handshake fails.
void maybe_upgrade_to_shm(int fd) {
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook)
        return;
    sdp::ScopedReentrancyGuard g;
    // Track the new fd in the remap table.
    sdp::state().fd_table.alloc(sd::kSocket, fd);

    // Pull both endpoints. AF_INET only; otherwise we just tracked the
    // fd and we're done.
    sockaddr_in local{}, peer{};
    socklen_t llen = sizeof(local), plen = sizeof(peer);
    if (REAL(getsockname)(fd, reinterpret_cast<sockaddr*>(&local), &llen) < 0
     || REAL(getpeername)(fd, reinterpret_cast<sockaddr*>(&peer), &plen) < 0)
        return;
    if (local.sin_family != AF_INET || peer.sin_family != AF_INET) return;

    auto local_ep = sdp::format_endpoint(reinterpret_cast<const sockaddr*>(&local));
    auto peer_ep  = sdp::format_endpoint(reinterpret_cast<const sockaddr*>(&peer));
    if (local_ep.empty() || peer_ep.empty()) return;

    auto conn = sdp::try_attach(local_ep, peer_ep, fd);
    if (conn) {
        // Record the blocking flag from O_NONBLOCK.
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) conn->nonblocking = (flags & O_NONBLOCK) != 0;
        sdp::conn_registry().install(fd, conn);
        // Cheap counter for tests / metrics.
        sd::MetricsRegistry::instance()
            .counter("socksdirect_lib_shm_conns_total",
                     "TCP fds upgraded to the SHM fast path")
            .inc();
    }
}

}  // namespace

SOCKSDIRECT_HOOK
int accept(int fd, struct sockaddr* a, socklen_t* l) {
    int c = REAL(accept)(fd, a, l);
    if (c >= 0) maybe_upgrade_to_shm(c);
    return c;
}

SOCKSDIRECT_HOOK
int accept4(int fd, struct sockaddr* a, socklen_t* l, int flags) {
    int c = REAL(accept4)(fd, a, l, flags);
    if (c >= 0) maybe_upgrade_to_shm(c);
    return c;
}

SOCKSDIRECT_HOOK
int connect(int fd, const struct sockaddr* a, socklen_t l) {
    int rc = REAL(connect)(fd, a, l);
    if (rc == 0) maybe_upgrade_to_shm(fd);
    return rc;
}

namespace {

// Wrappers around the futex(2) syscall — glibc doesn't expose it.
// Note: we use the *non-private* variants because the address lives
// in shared memory mapped into two separate processes. _PRIVATE is
// faster but only works within one address space.
inline long futex_wait(std::atomic<std::uint32_t>* uaddr, std::uint32_t val,
                       const struct timespec* timeout) {
    return ::syscall(SYS_futex, uaddr, FUTEX_WAIT, val, timeout,
                     nullptr, 0);
}
inline long futex_wake(std::atomic<std::uint32_t>* uaddr, int n) {
    return ::syscall(SYS_futex, uaddr, FUTEX_WAKE, n, nullptr,
                     nullptr, 0);
}

// Block-aware send via SHM. Holds c.send_mu so multiple threads in
// the application can call send(fd) concurrently without corrupting
// the SPSC ring.
ssize_t shm_send(sdp::ShmConn& c, const void* buf, std::size_t n, int flags) {
    if (n == 0) return 0;
    std::lock_guard<std::mutex> lk(c.send_mu);
    auto* ring = c.segment.ring_outbound();
    std::size_t total = 0;
    const char* p = static_cast<const char*>(buf);
    bool nonblock = c.nonblocking || (flags & MSG_DONTWAIT);
    int spin = 0;
    while (total < n) {
        std::size_t did = ring->send_some(p + total, n - total);
        if (did > 0) {
            // After publishing, wake any consumer FUTEX_WAITing.
            // Bump the wake counter (so a parallel WAIT detects the
            // value change) and call FUTEX_WAKE.
            auto* w = c.segment.wake_outbound();
            w->fetch_add(1, std::memory_order_release);
            futex_wake(w, 1);
            total += did;
            spin = 0;
            continue;
        }
        c.ring_full_blocks.fetch_add(1, std::memory_order_relaxed);
        if (nonblock) {
            if (total > 0) {
                c.bytes_sent.fetch_add(total, std::memory_order_relaxed);
                return static_cast<ssize_t>(total);
            }
            errno = EAGAIN;
            return -1;
        }
        if (spin < 100) {
            std::this_thread::yield();
            ++spin;
        } else {
            struct timespec ts{0, 1000};
            ::nanosleep(&ts, nullptr);
        }
    }
    c.bytes_sent.fetch_add(total, std::memory_order_relaxed);
    return static_cast<ssize_t>(total);
}

// Returns true if the underlying kernel TCP fd is hung up (POLLHUP /
// POLLERR / POLLNVAL). We check this when futex_wait times out so we
// detect a peer that crashed without running its libsd close hook
// (SIGKILL, segfault, etc.). Without this, the surviving side spins
// forever on recv.
namespace {
bool kernel_fd_hung_up(int fd) {
    struct pollfd pfd{ fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 0 };
    if (::poll(&pfd, 1, 0) <= 0) return false;
    return (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}
}  // namespace

ssize_t shm_recv(sdp::ShmConn& c, void* buf, std::size_t n, int flags) {
    if (n == 0) return 0;
    if (flags & MSG_PEEK) {
        errno = EOPNOTSUPP;
        return -1;
    }
    std::lock_guard<std::mutex> lk(c.recv_mu);
    auto* ring = c.segment.ring_inbound();
    bool nonblock = c.nonblocking || (flags & MSG_DONTWAIT);
    int spin = 0;
    auto* w = c.segment.wake_inbound();
    for (;;) {
        std::size_t did = ring->recv_some(buf, n);
        if (did > 0) {
            c.bytes_recv.fetch_add(did, std::memory_order_relaxed);
            return static_cast<ssize_t>(did);
        }
        if (ring->peer_closed() && ring->readable() == 0) {
            return 0;  // EOF
        }
        c.ring_empty_blocks.fetch_add(1, std::memory_order_relaxed);
        if (nonblock) {
            errno = EAGAIN;
            return -1;
        }
        // Detect a peer that crashed without running its close hook.
        // The kernel TCP fd will reflect POLLHUP once the peer's
        // socket is torn down; we treat that as ring closed.
        if (kernel_fd_hung_up(c.real_fd)) {
            ring->mark_closed();
            // Loop back to the top — recv_some may still return
            // the bytes that were already in the ring before the peer
            // crashed, then EOF on the next iteration.
            continue;
        }
        // First spin a little (sub-µs latency on the busy path).
        if (spin < 100) {
            std::this_thread::yield();
            ++spin;
            continue;
        }
        // Then go quiescent on the futex. We sample the wake counter
        // *before* re-checking the ring; if the producer publishes
        // between our re-check and the FUTEX_WAIT the wake counter
        // has changed and FUTEX_WAIT returns EAGAIN immediately.
        std::uint32_t w_val = w->load(std::memory_order_acquire);
        if (ring->readable() > 0) { spin = 0; continue; }
        if (ring->peer_closed())  { spin = 0; continue; }
        struct timespec ts{0, 100 * 1000 * 1000};   // 100 ms timeout
        futex_wait(w, w_val, &ts);
        spin = 0;
    }
}

}  // namespace

SOCKSDIRECT_HOOK
ssize_t send(int fd, const void* buf, size_t n, int flags) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            return shm_send(*c, buf, n, flags);
        }
    }
    return REAL(send)(fd, buf, n, flags);
}

SOCKSDIRECT_HOOK
ssize_t recv(int fd, void* buf, size_t n, int flags) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            return shm_recv(*c, buf, n, flags);
        }
    }
    return REAL(recv)(fd, buf, n, flags);
}

// write()/read() on sockets — many BSD apps (nginx, redis) use these
// instead of send()/recv(). Forward to the same SHM path.
DECLARE_REAL(ssize_t, write, int, const void*, size_t)
DECLARE_REAL(ssize_t, read,  int, void*,       size_t)

SOCKSDIRECT_HOOK
ssize_t write(int fd, const void* buf, size_t n) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            return shm_send(*c, buf, n, 0);
        }
    }
    return REAL(write)(fd, buf, n);
}

SOCKSDIRECT_HOOK
ssize_t read(int fd, void* buf, size_t n) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            return shm_recv(*c, buf, n, 0);
        }
    }
    return REAL(read)(fd, buf, n);
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
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook && msg) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            // Ancillary data on a SHM socket is meaningless: the
            // kernel socket isn't carrying the data, so there's
            // nowhere to attach it. Refuse cleanly.
            if (msg->msg_controllen > 0) {
                errno = EOPNOTSUPP;
                return -1;
            }
            ssize_t total = 0;
            for (int i = 0; i < msg->msg_iovlen; ++i) {
                const struct iovec& iov = msg->msg_iov[i];
                if (iov.iov_len == 0) continue;
                ssize_t r = shm_send(*c, iov.iov_base, iov.iov_len, flags);
                if (r < 0) return total > 0 ? total : -1;
                total += r;
                if (static_cast<std::size_t>(r) < iov.iov_len) {
                    return total;  // partial under non-blocking
                }
            }
            return total;
        }
    }
    return REAL(sendmsg)(fd, msg, flags);
}

SOCKSDIRECT_HOOK
ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    if (sdp::g_active.load(std::memory_order_acquire) && !sdp::g_in_hook && msg) {
        sdp::ScopedReentrancyGuard g;
        auto c = sdp::conn_registry().lookup(fd);
        if (c && c->segment.is_open()) {
            if (msg->msg_controllen > 0) msg->msg_controllen = 0;
            ssize_t total = 0;
            for (int i = 0; i < msg->msg_iovlen; ++i) {
                struct iovec& iov = msg->msg_iov[i];
                if (iov.iov_len == 0) continue;
                ssize_t r = shm_recv(*c, iov.iov_base, iov.iov_len, flags);
                if (r < 0) return total > 0 ? total : -1;
                if (r == 0) return total;  // EOF
                total += r;
                if (static_cast<std::size_t>(r) < iov.iov_len) break;
            }
            msg->msg_flags = 0;
            return total;
        }
    }
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
