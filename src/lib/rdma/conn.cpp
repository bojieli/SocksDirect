// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::rdma::Conn — RDMA inter-host data plane.
//
// Built only when -DSOCKSDIRECT_WITH_RDMA=ON. Without ibverbs, the
// CMake glue substitutes the "stub" translation unit at
// rdma/conn_stub.cpp which provides the same API but always reports
// "no RDMA available". That keeps the libsd build dependency-free by
// default.
//
// Status: design + plumbing. The QP setup, MR registration, and ring
// implementation are sketched here; end-to-end inter-host
// reproduction needs Mellanox (or compatible) hardware to run, which
// CI doesn't have. The legacy `libsd-legacy.so` path remains the
// route for paper-claim reproduction until this lands on bare metal.

#ifdef SOCKSDIRECT_WITH_RDMA

#include "socksdirect/rdma/conn.hpp"

#include "socksdirect/log.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <infiniband/verbs.h>
#include <memory>
#include <mutex>
#include <vector>

namespace socksdirect {
namespace rdma {

// Per-process verbs context. Singleton-style; we open the first
// active device unless `device` is named explicitly.
struct Verbs::Impl {
    ibv_context*  ctx        = nullptr;
    ibv_pd*       pd         = nullptr;
    ibv_cq*       cq         = nullptr;
    int           gid_index  = 3;          // RoCEv2 default
    std::mutex    lock;
};

Verbs::Verbs() : impl_(std::unique_ptr<Impl>(new Impl)) {}
Verbs::~Verbs() {
    std::lock_guard<std::mutex> g(impl_->lock);
    if (impl_->cq) ibv_destroy_cq(impl_->cq);
    if (impl_->pd) ibv_dealloc_pd(impl_->pd);
    if (impl_->ctx) ibv_close_device(impl_->ctx);
}

bool Verbs::open(const std::string& device_name) {
    std::lock_guard<std::mutex> g(impl_->lock);
    if (impl_->ctx) return true;

    int n = 0;
    ibv_device** devs = ibv_get_device_list(&n);
    if (!devs || n == 0) {
        LOG_DEBUG("ibv_get_device_list: no devices");
        return false;
    }
    ibv_device* picked = nullptr;
    for (int i = 0; i < n; ++i) {
        if (device_name == "auto" || device_name == ibv_get_device_name(devs[i])) {
            picked = devs[i];
            break;
        }
    }
    if (!picked) {
        ibv_free_device_list(devs);
        LOG_DEBUG("ibv: device %s not found", device_name.c_str());
        return false;
    }
    impl_->ctx = ibv_open_device(picked);
    ibv_free_device_list(devs);
    if (!impl_->ctx) return false;

    impl_->pd = ibv_alloc_pd(impl_->ctx);
    if (!impl_->pd) return false;
    // 4K-entry CQ is plenty for the Phase 3 connection scale.
    impl_->cq = ibv_create_cq(impl_->ctx, 4096, nullptr, nullptr, 0);
    return impl_->cq != nullptr;
}

bool   Verbs::is_open() const   { return impl_->ctx != nullptr; }
void*  Verbs::raw_pd()          { return impl_->pd; }
void*  Verbs::raw_cq()          { return impl_->cq; }
void*  Verbs::raw_dev_ctx()     { return impl_->ctx; }

// ---- Conn ---------------------------------------------------------------

struct Conn::Impl {
    Verbs*               verbs       = nullptr;
    ibv_qp*              qp          = nullptr;
    ibv_mr*              recv_mr     = nullptr;   // peer writes here
    ibv_mr*              send_mr     = nullptr;   // we write from here
    std::vector<uint8_t> recv_buf;                // ring memory
    std::vector<uint8_t> send_buf;
    std::atomic<uint64_t> recv_head{0};
    std::atomic<uint64_t> recv_tail{0};
    std::atomic<uint64_t> send_head{0};
    std::atomic<uint64_t> send_tail{0};
    std::atomic<bool>    peer_eof{false};
};

Conn::Conn(Verbs& verbs) : impl_(std::unique_ptr<Impl>(new Impl)) {
    impl_->verbs = &verbs;
}
Conn::~Conn() {
    if (!impl_) return;
    if (impl_->qp)      ibv_destroy_qp(impl_->qp);
    if (impl_->recv_mr) ibv_dereg_mr(impl_->recv_mr);
    if (impl_->send_mr) ibv_dereg_mr(impl_->send_mr);
}

bool Conn::init(std::size_t ring_size_bytes) {
    if (!impl_->verbs->is_open()) return false;
    auto* pd  = static_cast<ibv_pd*>(impl_->verbs->raw_pd());
    auto* cq  = static_cast<ibv_cq*>(impl_->verbs->raw_cq());

    impl_->recv_buf.resize(ring_size_bytes);
    impl_->send_buf.resize(ring_size_bytes);
    impl_->recv_mr = ibv_reg_mr(pd, impl_->recv_buf.data(), ring_size_bytes,
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    impl_->send_mr = ibv_reg_mr(pd, impl_->send_buf.data(), ring_size_bytes,
                                IBV_ACCESS_LOCAL_WRITE);
    if (!impl_->recv_mr || !impl_->send_mr) return false;

    ibv_qp_init_attr init{};
    init.send_cq          = cq;
    init.recv_cq          = cq;
    init.qp_type          = IBV_QPT_RC;
    init.sq_sig_all       = 0;
    init.cap.max_send_wr  = 128;
    init.cap.max_recv_wr  = 128;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    impl_->qp = ibv_create_qp(pd, &init);
    if (!impl_->qp) return false;

    // INIT state.
    ibv_qp_attr attr{};
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    int rc = ibv_modify_qp(impl_->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    return rc == 0;
}

QpInfo Conn::local_info() const {
    QpInfo q{};
    q.qp_num      = impl_->qp ? impl_->qp->qp_num : 0;
    q.rkey        = impl_->recv_mr ? impl_->recv_mr->rkey : 0;
    q.remote_addr = reinterpret_cast<uint64_t>(impl_->recv_buf.data());
    q.mtu         = IBV_MTU_4096;
    // GID query goes through the device context. Skipped here for
    // brevity; the legacy HERD code has the full pattern.
    return q;
}

bool Conn::connect(const QpInfo& peer) {
    if (!impl_->qp) return false;
    ibv_qp_attr attr{};
    // RTR
    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = static_cast<ibv_mtu>(peer.mtu);
    attr.dest_qp_num           = peer.qp_num;
    attr.rq_psn                = 0;
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12;
    attr.ah_attr.is_global     = 1;
    attr.ah_attr.dlid          = peer.lid;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = 1;
    attr.ah_attr.grh.dgid      = *reinterpret_cast<const ibv_gid*>(peer.gid);
    attr.ah_attr.grh.sgid_index = 3;
    attr.ah_attr.grh.hop_limit  = 64;
    if (ibv_modify_qp(impl_->qp, &attr,
            IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0)
        return false;

    // RTS
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(impl_->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) == 0;
}

// Stubs for the data path. Full implementation does:
//   send: copy into send_buf at tail, post RDMA WRITE to peer's
//         recv_buf at peer.tail, bump tail (locally + remotely via
//         a second WRITE to a head-tail header region).
//   recv: poll local recv_tail; copy out from recv_buf; bump head.
// Tracked under docs/MISSING_FEATURES.md.
std::size_t Conn::send_some(const void*, std::size_t) { return 0; }
std::size_t Conn::recv_some(void*,       std::size_t) { return 0; }
bool        Conn::peer_closed() const { return impl_->peer_eof.load(); }
void        Conn::mark_closed()       { impl_->peer_eof.store(true); }
bool        Conn::is_open() const     { return impl_->qp != nullptr; }

}  // namespace rdma
}  // namespace socksdirect

#else  /* SOCKSDIRECT_WITH_RDMA */

// Stub when RDMA support isn't compiled in.
#include "socksdirect/rdma/conn.hpp"
namespace socksdirect { namespace rdma {
struct Verbs::Impl {};
struct Conn::Impl  {};
Verbs::Verbs() : impl_(std::unique_ptr<Impl>(new Impl)) {}
Verbs::~Verbs() = default;
bool  Verbs::open(const std::string&) { return false; }
bool  Verbs::is_open() const { return false; }
void* Verbs::raw_pd()        { return nullptr; }
void* Verbs::raw_cq()        { return nullptr; }
void* Verbs::raw_dev_ctx()   { return nullptr; }
Conn::Conn(Verbs&) : impl_(std::unique_ptr<Impl>(new Impl)) {}
Conn::~Conn() = default;
bool Conn::init(std::size_t)        { return false; }
QpInfo Conn::local_info() const     { return {}; }
bool Conn::connect(const QpInfo&)   { return false; }
std::size_t Conn::send_some(const void*, std::size_t) { errno = ENOSYS; return 0; }
std::size_t Conn::recv_some(void*, std::size_t)       { errno = ENOSYS; return 0; }
bool Conn::peer_closed() const      { return true; }
void Conn::mark_closed()            {}
bool Conn::is_open() const          { return false; }
}}  // namespace

#endif  /* SOCKSDIRECT_WITH_RDMA */
