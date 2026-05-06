// SPDX-License-Identifier: Apache-2.0
//
// Per-connection SHM state for libsd's accelerated path.
//
// One ShmConn corresponds to one accelerated TCP connection. It holds:
//   - the ShmSegment (mmap'd backing memory shared with the peer)
//   - the role (creator/joiner) that determined ring direction
//   - blocking-mode flag inherited from the underlying fd
//   - the libsd-tracked vfd (so close() can find us)
//
// The ConnRegistry below maps real-fd -> ShmConn so the libsd hook
// layer can find us in O(1) on send/recv. We use unordered_map under
// a single mutex; the hot path (send/recv) doesn't take the mutex
// once the entry is found because the ShmConn struct itself is
// stable for the connection's lifetime.

#ifndef SOCKSDIRECT_LIB_SHM_CONN_HPP_
#define SOCKSDIRECT_LIB_SHM_CONN_HPP_

#include "socksdirect/shm_segment.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unordered_map>
#include <vector>

namespace socksdirect {
namespace preload {

struct ShmConn {
    ShmSegment segment;
    std::uint64_t key = 0;
    bool nonblocking = false;
    int  real_fd = -1;
    // Set when our side has done shutdown(SHUT_WR) or close().
    std::atomic<bool> sent_eof{false};

    // Per-conn metric counters (libsd-process-local; aggregated by
    // the dump-state ctl op when the lib pings the monitor).
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_recv{0};
    std::atomic<std::uint64_t> ring_full_blocks{0};   // send blocked
    std::atomic<std::uint64_t> ring_empty_blocks{0};  // recv blocked

    // Producer / consumer mutexes. The underlying ShmRing is SPSC —
    // two threads in the same process calling send() on the same fd
    // concurrently would corrupt the producer side. Per-direction
    // mutexes serialize the local end; the peer process holds its
    // own pair independently. Mutex acquisition is fast (memcpy +
    // atomic store inside) and contention matters only when an
    // application fans send()s across threads on the same fd.
    std::mutex send_mu;
    std::mutex recv_mu;

    // eventfd written by the consumer ring side ("we drained data") and
    // by the producer side ("we published data"). Used by epoll_ctl
    // to deliver readability events without polling. -1 if epoll
    // hasn't been requested for this fd.
    int notify_fd = -1;
};

class ConnRegistry {
public:
    void install(int fd, std::shared_ptr<ShmConn> c) {
        std::lock_guard<std::mutex> g(mu_);
        by_fd_[fd] = std::move(c);
    }

    std::shared_ptr<ShmConn> lookup(int fd) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_fd_.find(fd);
        if (it == by_fd_.end()) return nullptr;
        return it->second;
    }

    // Copy the registry's current ShmConn pointers under the mutex.
    // Used by the watchdog (which doesn't want to hold the mutex
    // while doing kill() syscalls) and by the metrics dumper.
    std::vector<std::shared_ptr<ShmConn>> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::shared_ptr<ShmConn>> out;
        out.reserve(by_fd_.size());
        for (const auto& kv : by_fd_) out.push_back(kv.second);
        return out;
    }

    std::shared_ptr<ShmConn> remove(int fd) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_fd_.find(fd);
        if (it == by_fd_.end()) return nullptr;
        auto out = std::move(it->second);
        by_fd_.erase(it);
        return out;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return by_fd_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<ShmConn>> by_fd_;
};

// Free helpers in src/lib/shm_conn.cpp.
//
// `try_attach` does the monitor handshake for the (local, peer) pair
// and (on success) opens the SHM segment. Returns nullptr if the pair
// shouldn't be accelerated (e.g. monitor unreachable, peer not
// preloaded). On nullptr, the caller falls back to plain TCP.
//
// `format_endpoint` builds the canonical "ip:port" string used as the
// handshake key. We use IPv4 only for now.

std::string format_endpoint(const struct ::sockaddr* sa);

std::shared_ptr<ShmConn> try_attach(const std::string& local_ep,
                                    const std::string& peer_ep,
                                    int real_fd);

ConnRegistry& conn_registry();

}  // namespace preload
}  // namespace socksdirect

#endif  // SOCKSDIRECT_LIB_SHM_CONN_HPP_
