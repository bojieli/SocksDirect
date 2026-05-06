// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::ShmHandshake — control-plane protocol that lets two
// preloaded processes discover each other and agree on a shared-memory
// key for the data-plane fast path.
//
// This is the *negotiation* layer. The actual SHM ring is allocated +
// mapped by the lib using the key returned here; the ring semantics
// live in src/lib/shm_ring.cpp (Phase 3 follow-up). The handshake is
// stand-alone testable today.
//
// Wire protocol (lives on top of MonitorIpc, so the same op-dispatch
// surface socksdirect-ctl uses):
//
//   client → monitor:  shm-register {peer_addr, peer_port, my_addr, my_port}
//   monitor → client:  ok lines: ["shm_key=<u64>", "role=<creator|joiner>"]
//
// "creator" is whichever side calls register first; the monitor
// allocates a fresh key and remembers the (peer_addr,peer_port) pair.
// "joiner" reuses the existing key.
//
// Security:
//   - Each request is annotated with the caller's pid (the monitor
//     sees this via SO_PEERCRED on the unix socket).
//   - Keys are 64-bit random (per-process collision-resistant).
//   - Memory unlinked when both sides have called shm-unregister or
//     when both pids have exited.

#ifndef SOCKSDIRECT_SHM_HANDSHAKE_HPP_
#define SOCKSDIRECT_SHM_HANDSHAKE_HPP_

#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <utility>

namespace socksdirect {

class ShmHandshakeRegistry {
public:
    // Identify a connection by the (lower-addr:lower-port,
    // higher-addr:higher-port) tuple so both sides converge on the
    // same key regardless of who calls first.
    struct ConnId {
        std::string a;       // lower endpoint, "ip:port" form
        std::string b;       // higher endpoint, "ip:port" form

        bool operator<(const ConnId& o) const {
            if (a != o.a) return a < o.a;
            return b < o.b;
        }
    };

    static ConnId make(const std::string& side1, const std::string& side2) {
        return side1 < side2 ? ConnId{side1, side2} : ConnId{side2, side1};
    }

    enum class Role { kCreator, kJoiner };

    struct Result {
        uint64_t key;
        Role role;
        // Pids of both sides, after both have registered. Empty if the
        // peer hasn't shown up yet.
        int pid_a = 0;
        int pid_b = 0;
    };

    Result register_endpoint(const ConnId& id, int caller_pid) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = active_.find(id);
        if (it == active_.end()) {
            Entry e{};
            e.key = next_key();
            e.creator_pid = caller_pid;
            it = active_.emplace(id, e).first;
            Result out;
            out.key = e.key; out.role = Role::kCreator;
            out.pid_a = caller_pid; out.pid_b = 0;
            return out;
        }
        // Joiner. Record the pid; second registration completes the pair.
        if (it->second.joiner_pid == 0) {
            it->second.joiner_pid = caller_pid;
        }
        Result out;
        out.key = it->second.key; out.role = Role::kJoiner;
        out.pid_a = it->second.creator_pid; out.pid_b = it->second.joiner_pid;
        return out;
    }

    // Returns true if the registry had this endpoint and removed it.
    bool unregister_endpoint(const ConnId& id) {
        std::lock_guard<std::mutex> g(mu_);
        return active_.erase(id) > 0;
    }

    // Drop everything keyed by this pid (either side). Called when a
    // process exits via SO_PEERCRED hangup detection in the daemon.
    std::size_t reap_pid(int pid) {
        std::lock_guard<std::mutex> g(mu_);
        std::size_t removed = 0;
        for (auto it = active_.begin(); it != active_.end();) {
            if (it->second.creator_pid == pid || it->second.joiner_pid == pid) {
                it = active_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    std::size_t live_pairs() const {
        std::lock_guard<std::mutex> g(mu_);
        return active_.size();
    }

    // For tests — let the caller seed the RNG.
    void seed_for_test(uint64_t s) { rng_.seed(s); }

private:
    struct Entry {
        uint64_t key;
        int creator_pid;
        int joiner_pid;
    };

    uint64_t next_key() {
        // Avoid 0 as a key (used as "no entry" sentinel).
        uint64_t k = 0;
        while (k == 0) k = rng_();
        return k;
    }

    mutable std::mutex mu_;
    std::map<ConnId, Entry> active_;
    std::mt19937_64 rng_{std::random_device{}()};
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_SHM_HANDSHAKE_HPP_
