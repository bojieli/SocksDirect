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

    union
    {
        uint8_t  rdma_parm_buffer[sizeof(qp_info_t)];
        qp_info_t peer_qpinfo;
    };
    qp_info_t my_qpinfo;

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

    

}

#undef DEBUGON