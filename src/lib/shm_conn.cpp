// SPDX-License-Identifier: Apache-2.0
//
// SHM-conn registry + monitor handshake glue. Lives in libsd's hot
// path, so we keep it simple and avoid heap traffic on accept/connect.

#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"

#include "socksdirect/log.hpp"
#include "socksdirect/monitor_ipc.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <unistd.h>

namespace socksdirect {
namespace preload {

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
    return conn;
}

}  // namespace preload
}  // namespace socksdirect
