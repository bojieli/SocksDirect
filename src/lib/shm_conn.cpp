// SPDX-License-Identifier: Apache-2.0
//
// SHM-conn registry + monitor handshake glue. Lives in libsd's hot
// path, so we keep it simple and avoid heap traffic on accept/connect.

#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"

#include "socksdirect/log.hpp"
#include "socksdirect/monitor_ipc.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <linux/futex.h>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace socksdirect {
namespace preload {

// Per-process watchdog: scans the conn registry every ~50 ms and
// checks each conn's peer_pid via kill(pid, 0). If the peer is gone
// (kill returns ESRCH or EPERM), we mark our inbound ring closed and
// FUTEX_WAKE so a parked recv returns EOF in tens of milliseconds
// rather than waiting for the kernel TCP stack to surface POLLHUP
// (which can take many seconds on a quiet loopback).
namespace {

class Watchdog {
public:
    static Watchdog& instance() {
        static Watchdog w;
        return w;
    }

    // Idempotent. Calls only the first one start the thread.
    void ensure_started() {
        if (started_.exchange(true, std::memory_order_acq_rel)) return;
        std::thread([this]() { this->run(); }).detach();
    }

private:
    void run() {
        ::pthread_setname_np(::pthread_self(), "sd-watchdog");
        for (;;) {
            sweep_once();
            struct timespec ts{0, 50 * 1000 * 1000};   // 50 ms
            ::nanosleep(&ts, nullptr);
        }
    }

    void sweep_once() {
        auto& reg = conn_registry();
        // Snapshot the registry under its lock, then operate on the
        // shared_ptrs outside it so we don't hold the registry mutex
        // while doing kill() syscalls.
        std::vector<std::shared_ptr<ShmConn>> snap = reg.snapshot();
        for (auto& c : snap) {
            if (!c || !c->segment.is_open()) continue;
            std::int32_t pid = c->segment.peer_pid();
            if (pid <= 0) continue;
            // kill(pid, 0): returns 0 if process exists. ESRCH if not,
            // EPERM if we lack permission (treated as alive — the
            // process exists, just not signalable).
            if (::kill(pid, 0) == 0) continue;
            if (errno == EPERM) continue;
            // Peer is gone. Mark our inbound ring closed, wake any
            // parked recv, and shm_unlink the segment ourselves —
            // the dead peer's libsd never decremented its refcount,
            // so without unlinking here the segment would persist
            // until reboot.
            auto* ring = c->segment.ring_inbound();
            ring->mark_closed();
            auto* w = c->segment.wake_inbound();
            w->fetch_add(1, std::memory_order_release);
            ::syscall(SYS_futex, w, FUTEX_WAKE, INT_MAX, nullptr,
                      nullptr, 0);
            // Best-effort unlink. ENOENT is fine (race with peer
            // who managed to unlink before crashing, or another
            // watchdog).
            ShmSegment::unlink_by_key(c->key);
            LOG_DEBUG("watchdog: peer pid=%d gone; marked ring closed "
                      "+ unlinked (real_fd=%d key=%016llx)",
                      pid, c->real_fd,
                      static_cast<unsigned long long>(c->key));
        }
    }

    std::atomic<bool> started_{false};
};

}  // namespace

ConnRegistry& conn_registry() {
    static ConnRegistry reg;
    return reg;
}

std::string format_endpoint(const struct ::sockaddr* sa) {
    if (!sa) return {};
    if (sa->sa_family != AF_INET) return {};
    auto* in = reinterpret_cast<const ::sockaddr_in*>(sa);
    char buf[64];
    char ip[INET_ADDRSTRLEN];
    if (!::inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip))) return {};
    std::snprintf(buf, sizeof(buf), "%s:%u", ip,
                  static_cast<unsigned>(::ntohs(in->sin_port)));
    return buf;
}

namespace {

// Canonical key derivation: sort the two endpoints lexicographically.
struct CanonicalPair {
    std::string a, b;
};

CanonicalPair canonical(const std::string& x, const std::string& y) {
    return x < y ? CanonicalPair{x, y} : CanonicalPair{y, x};
}

// Talk to the monitor and request a SHM key for the (local, peer) pair.
// Returns 0 on success and fills `key` + `is_creator`.
int request_key_from_monitor(const std::string& local_ep,
                             const std::string& peer_ep,
                             std::uint64_t* key,
                             bool* is_creator) {
    // Resolve the monitor socket path the same way preload_init does.
    std::string sock = state().config.get_string(
        "monitor", "control_socket", kMonitorCtlSocketDefault);
    int fd = connect_unix(sock);
    if (fd < 0) return -1;

    auto pair = canonical(local_ep, peer_ep);
    char pidbuf[16];
    std::snprintf(pidbuf, sizeof(pidbuf), "%d", static_cast<int>(::getpid()));

    CtlRequest req;
    req.op = "shm-register";
    req.args.push_back(pair.a);
    req.args.push_back(pair.b);
    req.args.push_back(pidbuf);
    if (!write_all(fd, encode_request(req))) {
        ::close(fd); return -1;
    }
    std::string line = read_line(fd);
    ::close(fd);
    CtlResponse resp;
    if (!decode_response(line, resp) || !resp.ok) return -1;

    bool got_key = false, got_role = false;
    for (const auto& l : resp.lines) {
        if (l.compare(0, 8, "shm_key=") == 0) {
            *key = std::strtoull(l.c_str() + 8, nullptr, 10);
            got_key = true;
        } else if (l == "role=creator") {
            *is_creator = true;  got_role = true;
        } else if (l == "role=joiner") {
            *is_creator = false; got_role = true;
        }
    }
    if (!got_key || !got_role || *key == 0) return -1;
    return 0;
}

}  // namespace

std::shared_ptr<ShmConn> try_attach(const std::string& local_ep,
                                    const std::string& peer_ep,
                                    int real_fd) {
    // 127.0.0.1-only acceleration for v0; remote peers fall back to
    // RDMA in the legacy library and to plain TCP in the new one.
    auto is_loopback = [](const std::string& ep) {
        return ep.compare(0, 10, "127.0.0.1:") == 0
            || ep.compare(0, 10, "127.0.0.0:") == 0;
    };
    if (!is_loopback(local_ep) || !is_loopback(peer_ep)) {
        return nullptr;
    }

    std::uint64_t key = 0;
    bool is_creator = false;
    if (request_key_from_monitor(local_ep, peer_ep, &key, &is_creator) != 0) {
        LOG_DEBUG("shm-register failed for %s <-> %s; falling back to TCP",
                  local_ep.c_str(), peer_ep.c_str());
        return nullptr;
    }

    auto conn = std::make_shared<ShmConn>();
    conn->key = key;
    conn->real_fd = real_fd;
    auto role = is_creator ? ShmSegment::kRoleCreator : ShmSegment::kRoleJoiner;
    if (conn->segment.open(key, role) != 0) {
        LOG_DEBUG("shm_open(%016llx) failed: %s; falling back to TCP",
                  static_cast<unsigned long long>(key), std::strerror(errno));
        return nullptr;
    }
    // Both sides must have mmap'd the segment before either side
    // proceeds. Otherwise an early-closing creator can unlink the
    // segment before the joiner shm_open's it (asymmetric upgrade
    // bug seen in epoll tests). We wait up to 200 ms; on timeout we
    // tear down our side of the SHM and fall back to TCP — the peer
    // (presumably also gave up) does the same independently.
    if (!conn->segment.wait_for_peer(200)) {
        LOG_DEBUG("peer never attached for key=%016llx; falling back to TCP",
                  static_cast<unsigned long long>(key));
        // segment.close() unlinks if we were the only attacher.
        return nullptr;
    }
    LOG_INFO("shm-attached: fd=%d local=%s peer=%s key=%016llx role=%s",
             real_fd, local_ep.c_str(), peer_ep.c_str(),
             static_cast<unsigned long long>(key),
             is_creator ? "creator" : "joiner");
    state().shm_conns_total.fetch_add(1, std::memory_order_relaxed);
    // Start the watchdog (idempotent). Without this, a peer that
    // SIGKILLs leaves the surviving side spinning until POLLHUP
    // surfaces — which on quiet loopback can take seconds.
    Watchdog::instance().ensure_started();
    return conn;
}

}  // namespace preload
}  // namespace socksdirect
