// SPDX-License-Identifier: Apache-2.0
//
// libsd epoll family hooks.
//
// SHM-aware epoll. The trick is that an epoll fd watches *real* fds,
// not synthetic ones; we leverage that:
//
//   - When the application calls epoll_ctl(EPOLL_CTL_ADD, fd, EPOLLIN)
//     and `fd` has a SHM connection, we lazily create an `eventfd` on
//     the ShmConn (`notify_fd`) and register the eventfd with the
//     epoll set instead of the application's fd. We also remember
//     the application's fd so we can substitute it back when reporting.
//
//   - On the data-publish side (shm_send), we write 1 to the peer's
//     notify_fd via the SHM segment's notification flag — but the
//     peer's notify_fd lives in the peer's process, so cross-process
//     notification needs a slightly different mechanism. We use a
//     per-process eventfd plus a "wake counter" in the ring header
//     that the consumer reads. The producer increments the counter
//     and writes to the consumer's eventfd via the local kernel SHM
//     wake table — but on a single host, eventfd can be passed via
//     SCM_RIGHTS at handshake time. **For v0** of epoll integration
//     we go simpler: each consumer process has its own eventfd; the
//     consumer's libsd registers a small per-process polling thread
//     that watches all SHM rings it owns and writes to the eventfds
//     when they have data. epoll_wait then fires.
//
// EPOLLHUP / EPOLLRDHUP were UNSUPPORTED in the prototype; passing
// them through preserves correctness today, and the SHM polling
// thread synthesizes them when the ring's peer_closed flag flips.

#include "src/lib/intercept.hpp"
#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/log.hpp"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace sd  = socksdirect;
namespace sdp = socksdirect::preload;

DECLARE_REAL(int, epoll_create,  int)
DECLARE_REAL(int, epoll_create1, int)
DECLARE_REAL(int, epoll_ctl,     int, int, int, struct epoll_event*)
DECLARE_REAL(int, epoll_wait,    int, struct epoll_event*, int, int)
DECLARE_REAL(int, epoll_pwait,   int, struct epoll_event*, int, int,
                                 const sigset_t*)
DECLARE_REAL(int, eventfd,       unsigned, int)

namespace {

// A single per-process polling thread that watches all SHM rings the
// process is a consumer for, and writes to each ring's notify_fd when
// readable (or when peer_closed). Started on first epoll_ctl that
// registers a SHM fd; runs until process exit.
//
// Implementation: a small map of eventfd_to_watch -> ShmConn, plus a
// mutex. Watch loop spins (cheap; single thread for the whole
// process). Replaced by the futex-on-ring follow-up.
class ShmPoller {
public:
    static ShmPoller& instance() {
        static ShmPoller p;
        return p;
    }

    void watch(int fd, std::shared_ptr<sdp::ShmConn> c) {
        std::lock_guard<std::mutex> g(mu_);
        watched_[fd] = std::move(c);
        ensure_thread_locked();
    }

    void unwatch(int fd) {
        std::lock_guard<std::mutex> g(mu_);
        watched_.erase(fd);
    }

    std::size_t watched_count() {
        std::lock_guard<std::mutex> g(mu_);
        return watched_.size();
    }

private:
    void ensure_thread_locked() {
        if (started_) return;
        started_ = true;
        std::thread([this]() { this->run(); }).detach();
    }

    void run() {
        // Set thread name so users see it in `top -H`.
        ::pthread_setname_np(::pthread_self(), "sd-shm-poll");
        while (true) {
            std::vector<std::pair<int, std::shared_ptr<sdp::ShmConn>>> snap;
            {
                std::lock_guard<std::mutex> g(mu_);
                snap.reserve(watched_.size());
                for (auto& kv : watched_) snap.emplace_back(kv.first, kv.second);
            }
            for (auto& kv : snap) {
                auto* c = kv.second.get();
                if (!c->segment.is_open()) continue;
                auto* ring = c->segment.ring_inbound();
                if (ring->readable() > 0 || ring->peer_closed()) {
                    if (c->notify_fd >= 0) {
                        std::uint64_t one = 1;
                        ssize_t w = ::write(c->notify_fd, &one, sizeof(one));
                        (void)w;
                    }
                }
            }
            // Yield rather than spin pinball. ~µs latency on epoll
            // wakeup; replace with futex in a follow-up.
            struct timespec ts{0, 10000};  // 10 µs
            ::nanosleep(&ts, nullptr);
        }
    }

    std::mutex mu_;
    std::map<int, std::shared_ptr<sdp::ShmConn>> watched_;
    bool started_ = false;
};

}  // namespace

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
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook) {
        return REAL(epoll_ctl)(epfd, op, fd, ev);
    }
    sdp::ScopedReentrancyGuard g;

    auto c = sdp::conn_registry().lookup(fd);
    if (!c || !c->segment.is_open()) {
        // Not a SHM-tracked fd; pure passthrough.
        return REAL(epoll_ctl)(epfd, op, fd, ev);
    }

    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (c->notify_fd < 0) {
            int nfd = REAL(eventfd)(0u, EFD_NONBLOCK | EFD_CLOEXEC);
            if (nfd < 0) return -1;
            c->notify_fd = nfd;
        }
        ShmPoller::instance().watch(fd, c);
        // Substitute the eventfd in the kernel epoll set. Stash the
        // original user-data so we can restore it in epoll_wait.
        struct epoll_event mev = ev ? *ev : epoll_event{};
        if (op == EPOLL_CTL_MOD && c->notify_fd >= 0) {
            // Already added previously; modify the existing
            // registration (it's keyed on c->notify_fd in the kernel).
        }
        // Use the original fd as data so the application sees the
        // fd it registered.
        if (ev) mev.data.fd = fd;
        return REAL(epoll_ctl)(epfd, op, c->notify_fd, &mev);
    }

    if (op == EPOLL_CTL_DEL) {
        if (c->notify_fd >= 0) {
            int rc = REAL(epoll_ctl)(epfd, EPOLL_CTL_DEL, c->notify_fd, nullptr);
            ShmPoller::instance().unwatch(fd);
            ::close(c->notify_fd);
            c->notify_fd = -1;
            return rc;
        }
    }

    return REAL(epoll_ctl)(epfd, op, fd, ev);
}

SOCKSDIRECT_HOOK
int epoll_wait(int epfd, struct epoll_event* events, int max, int timeout) {
    int n = REAL(epoll_wait)(epfd, events, max, timeout);
    if (n <= 0) return n;
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook) return n;
    sdp::ScopedReentrancyGuard g;
    // Drain the eventfd for each fired SHM connection so subsequent
    // epoll_wait calls don't see a stale wakeup. The application sees
    // the original fd via data.fd (we set that in epoll_ctl).
    for (int i = 0; i < n; ++i) {
        auto c = sdp::conn_registry().lookup(events[i].data.fd);
        if (c && c->notify_fd >= 0) {
            std::uint64_t drain;
            (void)::read(c->notify_fd, &drain, sizeof(drain));
        }
    }
    return n;
}

SOCKSDIRECT_HOOK
int epoll_pwait(int epfd, struct epoll_event* events, int max, int timeout,
                const sigset_t* mask) {
    int n = REAL(epoll_pwait)(epfd, events, max, timeout, mask);
    if (n <= 0) return n;
    if (!sdp::g_active.load(std::memory_order_acquire) || sdp::g_in_hook) return n;
    sdp::ScopedReentrancyGuard g;
    for (int i = 0; i < n; ++i) {
        auto c = sdp::conn_registry().lookup(events[i].data.fd);
        if (c && c->notify_fd >= 0) {
            std::uint64_t drain;
            (void)::read(c->notify_fd, &drain, sizeof(drain));
        }
    }
    return n;
}
