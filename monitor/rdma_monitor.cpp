//
// Created by ctyi on 7/17/18.
//
#include "rdma_monitor.h"
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include "../common/helper.h"
#include "../common/rdma.h"
#include "../common/metaqueue.h"
#include "../common/darray.hpp"

static int rdma_sock_fd;
static rdma_pack rdma_monitor_context;
darray_t<remote_process_t, 32> rdma_processes;
int rdma_processes_seq;
ibv_cq *shared_cq;

void create_rdma_socket()
{
    rdma_sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (rdma_sock_fd == -1)
        FATAL("Failed to init RDMA setup sock, %s", strerror(errno));
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(RDMA_SOCK_PORT);
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    if (bind(rdma_sock_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to bind RDMA setup sock, %s", strerror(errno));
    if (listen(rdma_sock_fd, 10) < 0)
        FATAL("Failed to listen on RDMA setup sock, %s", strerror(errno));
}
#undef DEBUGON
#define DEBUGON 1

void rdma_init()
{
    //several things
    //1. Init a large buffer
    //2. Allocate PD
    //3. enumerate device and get dev id

    //enumerate device
    enum_dev(&rdma_monitor_context);
    //allocate pd
    rdma_monitor_context.ibv_pd = ibv_alloc_pd(rdma_monitor_context.ib_ctx);
    if (rdma_monitor_context.ibv_pd == nullptr)
        FATAL("Failed to create Protected Domain for RDMA");

    //allocate a large buffer
    rdma_monitor_context.MR_ptr = memalign(4096, (size_t)MAX_RDMA_CONN_NUM * metaqueue_t::get_sharememsize());
    if (rdma_monitor_context.MR_ptr == nullptr)
        FATAL("Failed to create a large buffer");
    //reg it to NIC MR

    int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    rdma_monitor_context.buf_mr = ibv_reg_mr(rdma_monitor_context.ibv_pd, rdma_monitor_context.MR_ptr,
                                         (size_t)MAX_RDMA_CONN_NUM * metaqueue_t::get_sharememsize(),ib_flags);
    if (rdma_monitor_context.buf_mr == nullptr)
        FATAL("Failed to reg MR for RDMA");

    //create a shared cq
    shared_cq = ibv_create_cq(rdma_monitor_context.ib_ctx, QPRQDepth+QPSQDepth, nullptr, nullptr, 0);

    if (shared_cq == nullptr)
        FATAL("Failed to create a shared CQ");

    create_rdma_socket();

    rdma_processes.init();
    rdma_processes_seq = 0;
    DEBUG("RDMA Init finished!");
}

bool try_new_rdma()
{
    int peerfd;
    if ((peerfd = accept(rdma_sock_fd, NULL, NULL)) == -1)
    {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
             return false;
        else
            FATAL("RDMA accept remote process failed with err: %s", strerror(errno));
    }


    union {
        uint8_t sendbuf[sizeof(qp_info_t)];
        qp_info_t my_qpinfo;
    };

    my_qpinfo.qid=rdma_processes_seq;
    ++rdma_processes_seq;
    remote_process_t peer_qp;
    peer_qp.qid=my_qpinfo.qid;
    uint32_t proc_idx=rdma_processes.add(peer_qp);

    ibv_qp_init_attr myqp_attr;
    myqp_attr.send_cq = shared_cq;
    myqp_attr.recv_cq = shared_cq;
    myqp_attr.qp_type = IBV_QPT_RC;
    myqp_attr.cap.max_send_wr = QPSQDepth;
    myqp_attr.cap.max_recv_wr = QPRQDepth;
    myqp_attr.cap.max_send_sge = 1;
    myqp_attr.cap.max_recv_sge = 1;
    myqp_attr.cap.max_inline_data = QPMaxInlineData;

    rdma_processes[proc_idx].myqp = ibv_create_qp(rdma_monitor_context.ibv_pd, &myqp_attr);
    if (rdma_processes[proc_idx].myqp == nullptr)
        FATAL("Failed to create QP");

    //change the QP to init state
    ibv_qp_attr myqp_stateupdate_attr;
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_INIT;
    myqp_stateupdate_attr.pkey_index = 0;
    myqp_stateupdate_attr.port_num = rdma_monitor_context.dev_port_id;
    myqp_stateupdate_attr.qp_access_flags =
            IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(rdma_processes[proc_idx].myqp, &myqp_stateupdate_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        FATAL("Failed to set QP to init");

    my_qpinfo.qpn = rdma_processes[proc_idx].myqp->qp_num;
    my_qpinfo.RoCE_gid = rdma_monitor_context.RoCE_gid;
    my_qpinfo.rkey = rdma_monitor_context.MR_rkey;
    my_qpinfo.remote_buf_addr = (uintptr_t)(rdma_monitor_context.MR_ptr + 2 * proc_idx * metaqueue_t::get_sharememsize());
    my_qpinfo.buf_size = 2 * proc_idx * metaqueue_t::get_sharememsize();
    my_qpinfo.port_lid = rdma_monitor_context.port_lid;

    //send QP info to peer
    ssize_t left_byte=sizeof(qp_info_t);
    int currptr=0;
    while (left_byte > 0)
    {
        ssize_t curr_sent_byte = send(peerfd, &sendbuf[currptr], left_byte, MSG_WAITALL);
        currptr += curr_sent_byte;
        left_byte -= curr_sent_byte;
    }

    //Try to receive the info of remote QP
    union {
        uint8_t recvbuf[sizeof(qp_info_t)];
        qp_info_t peer_qpinfo;
    };
    left_byte = sizeof(qp_info_t);
    currptr=0;
    while (left_byte > 0)
    {
        ssize_t  curr_recv_byte = recv(peerfd, &recvbuf[currptr], left_byte, MSG_WAITALL);
        currptr += curr_recv_byte;
        left_byte -= curr_recv_byte;
    }

    //change state to RTR
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_RTR;
    myqp_stateupdate_attr.path_mtu = IBV_MTU_4096;
    myqp_stateupdate_attr.dest_qp_num = peer_qpinfo.qpn;
    myqp_stateupdate_attr.rq_psn = 3185;

    myqp_stateupdate_attr.ah_attr.is_global = 1;
    myqp_stateupdate_attr.ah_attr.dlid = peer_qpinfo.port_lid;
    myqp_stateupdate_attr.ah_attr.sl = 0;
    myqp_stateupdate_attr.ah_attr.src_path_bits = 0;
    myqp_stateupdate_attr.ah_attr.port_num = rdma_monitor_context.dev_port_id;  // Local port!

    auto& grh = myqp_stateupdate_attr.ah_attr.grh;
    grh.dgid.global.interface_id = peer_qpinfo.RoCE_gid.global.interface_id;
    grh.dgid.global.subnet_prefix = peer_qpinfo.RoCE_gid.global.subnet_prefix;
    grh.sgid_index = 0;
    grh.hop_limit = 1;

    myqp_stateupdate_attr.max_dest_rd_atomic = 16;
    myqp_stateupdate_attr.min_rnr_timer = 12;
    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(rdma_processes[proc_idx].myqp, &myqp_stateupdate_attr, rtr_flags)) {
        FATAL("Failed to modify QP from INIT to RTR");
    }

    //set QP to RTS
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_RTS;
    myqp_stateupdate_attr.sq_psn = 3185;

    int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                                   IBV_QP_MAX_QP_RD_ATOMIC;
    myqp_stateupdate_attr.timeout = 14;
    myqp_stateupdate_attr.retry_cnt = 7;
    myqp_stateupdate_attr.rnr_retry = 7;
    myqp_stateupdate_attr.max_rd_atomic = 16;
    myqp_stateupdate_attr.max_dest_rd_atomic = 16;
        rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                     IBV_QP_MAX_QP_RD_ATOMIC;


    if (ibv_modify_qp(rdma_processes[proc_idx].myqp, &myqp_stateupdate_attr, rts_flags)) {
        FATAL("Failed to modify QP from RTR to RTS");
    }
    return true;


}

#undef DEBUGON