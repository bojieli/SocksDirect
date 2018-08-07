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

    rdma_processes[proc_idx].myqp = rdma_create_qp(shared_cq, &rdma_monitor_context);

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

    rdma_connect_remote_qp(rdma_processes[proc_idx].myqp,&rdma_monitor_context, &peer_qpinfo);
    //save rkey and buf addr
    rdma_processes[proc_idx].rkey = peer_qpinfo.rkey;
    rdma_processes[proc_idx].remote_buf_ptr = peer_qpinfo.remote_buf_addr;
    return true;


}

#undef DEBUGON