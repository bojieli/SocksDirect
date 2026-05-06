// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::rdma::Conn — RDMA connection abstraction.
//
// Goal: provide the same Send/Recv surface as the SHM connection so
// libsd's send/recv hooks can dispatch on connection type without
// caring about transport. The intent is for socket_api.cpp to look
// at a per-fd descriptor and call either ShmConn::send_some or
// rdma::Conn::send_some.
//
// Wire model (Phase 3 RDMA, version 0):
//
//   - Each connection allocates two RDMA-registered ring buffers
//     mirroring the SHM layout: one for outbound (us → peer), one
//     for inbound (peer → us). They sit in the libsd-process's
//     virtual address space, not in shared memory.
//   - Ring producer (sender) does an RDMA WRITE WITH IMMEDIATE to
//     the peer's *inbound* ring buffer. The immediate value carries
//     the byte offset + length so the peer's CQ poll sees what
//     was published.
//   - Ring consumer (receiver) polls its receive completion queue;
//     each completion delivers one immediate. The receiver's
//     pre-posted RECV WRs cycle through a fixed pool sized to
//     amortize CQ doorbell costs (matches HERD's design).
//
// Connection setup goes through the monitor's `rdma-register` op:
//
//   client → monitor:  rdma-register <local_ep> <peer_ep> <pid> <qpinfo_hex>
//   monitor → client:  ok lines:
//                        peer_qpinfo=<hex>      (after both have registered)
//                        role=creator|joiner
//
// Both peers can then transition their QP to RTR using the
// peer_qpinfo. The monitor handshake is identical to the SHM one;
// the differentiator is the QP info exchange.
//
// Why not vanilla rdma_cm (RDMA-CM): we already have the monitor
// brokering connection setup; reusing rdma_cm would add another
// listener socket per host, which makes failure handling harder.
// The hand-rolled QP exchange is ~50 lines and removes a deps cone.

#ifndef SOCKSDIRECT_RDMA_CONN_HPP_
#define SOCKSDIRECT_RDMA_CONN_HPP_

#include "socksdirect/rdma/qp_info.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace socksdirect {
namespace rdma {

// Forward declare so we can keep ibverbs out of the public header.
class Verbs;
class CompChannel;

// Per-connection state. Construction allocates the QP and registers
// MRs; transition() drives the state machine after the peer's info
// arrives.
class Conn {
public:
    explicit Conn(Verbs& verbs);
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    // Step 1: allocate QP + post initial RECVs. Must succeed before
    // we exchange info via the monitor.
    bool init(std::size_t ring_size_bytes);

    // Returns the QP info to ship to the peer via the monitor.
    QpInfo local_info() const;

    // Step 2: drive the QP from INIT → RTR → RTS using the peer's
    // info. Blocking but bounded (~µs).
    bool connect(const QpInfo& peer);

    // Same surface as ShmRing::send_some / recv_some.
    std::size_t send_some(const void* buf, std::size_t len);
    std::size_t recv_some(void* buf,       std::size_t len);
    bool peer_closed() const;
    void mark_closed();

    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Verbs — process-wide RDMA context. One per pd/cq/srq family.
// The ZeroCopyClient pattern: open the device on first use, fall
// back gracefully if no RDMA NIC is available.
class Verbs {
public:
    Verbs();
    ~Verbs();
    bool open(const std::string& device_name = "auto");
    bool is_open() const;

    // Internal accessor for Conn::Impl. We keep the type opaque to
    // avoid leaking ibverbs.h to this header.
    void* raw_pd();
    void* raw_cq();
    void* raw_dev_ctx();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rdma
}  // namespace socksdirect

#endif  // SOCKSDIRECT_RDMA_CONN_HPP_
