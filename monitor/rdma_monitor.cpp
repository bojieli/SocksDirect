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
#include "sock_monitor.h"
#include "process.h"

static int rdma_sock_fd = -1;
static rdma_pack rdma_monitor_context;
darray_t<remote_process_t, 32> rdma_processes;
int rdma_processes_seq;
ibv_cq *shared_recv_cq;

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
#define DEBUGON 0

void rdma_init()
{
    //several things
    //1. Init a large buffer
    //2. Allocate PD
    //3. enumerate device and get dev id

    //enumerate device
    if (enum_dev(&rdma_monitor_context)) {
        DEBUG("No RDMA devices present, initialization skipped");
        return;
    }
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

    //create a shared send_cq
    shared_recv_cq = ibv_create_cq(rdma_monitor_context.ib_ctx, QPRQDepth+QPSQDepth, nullptr, nullptr, 0);

    if (shared_recv_cq == nullptr)
        FATAL("Failed to create a shared CQ");

    create_rdma_socket();

    rdma_processes.init();
    rdma_processes_seq = 0;
    DEBUG("RDMA Init finished!");
}

void test_rdma_recv(int proc_idx) {
    //Actually we do nothing here and run an infinity loop
    metaqueue_t * metaqueue_ptr = &rdma_processes[proc_idx].metaqueue;
    metaqueue_ctl_element ele;
    unsigned int cnt=0;
    while (true)
    {
        while (!metaqueue_ptr->q[1].pop_nb(ele));
        ++cnt;
        if (cnt % 10000000 == 0)
            printf("10M\n");
        //printf("%x\n", *((int *)&ele.raw));
    }
}

bool try_new_rdma()
{
    if (rdma_sock_fd == -1) { // rdma is not initialized
        return false;
    }

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
    //create a shared send_cq
    rdma_processes[proc_idx].send_cq = ibv_create_cq(rdma_monitor_context.ib_ctx,
            QPRQDepth+QPSQDepth, nullptr, nullptr, 0);
    if (rdma_processes[proc_idx].send_cq == nullptr)
        FATAL("Failed to create send CQ.");
    rdma_processes[proc_idx].myqp = rdma_create_qp(rdma_processes[proc_idx].send_cq,
            shared_recv_cq, &rdma_monitor_context);

    my_qpinfo.qpn = rdma_processes[proc_idx].myqp->qp_num;
    my_qpinfo.RoCE_gid = rdma_monitor_context.RoCE_gid;
    my_qpinfo.rkey = rdma_monitor_context.buf_mr->rkey;

    //The metaqueue contain for lockless_q, each lockless_q contain 256 element and a return flag
    //We could notice that only send buffer is required for RDMA.
     // The buf addr stores the base addr of the whole remote structure, include both send and recv
    my_qpinfo.remote_buf_addr =
            (uintptr_t)(rdma_monitor_context.MR_ptr + proc_idx * metaqueue_t::get_sharememsize());
    my_qpinfo.buf_size =  metaqueue_t::get_sharememsize();
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

    //The next thing we need to do is to init the metaqueue
    rdma_processes[proc_idx].metaqueue.init_memlayout((uint8_t *)my_qpinfo.remote_buf_addr,0);
    rdma_processes[proc_idx].metaqueue.mem_init();
    rdma_processes[proc_idx].metaqueue.initRDMA(rdma_processes[proc_idx].myqp,
                                                rdma_processes[proc_idx].send_cq,
                                                rdma_monitor_context.buf_mr->lkey,
                                                rdma_processes[proc_idx].rkey,
                                                rdma_processes[proc_idx].remote_buf_ptr,
                                                0);
    //We should block here until peer QP created

    sendbuf[0]=1;
    ssize_t curr_byte;
    do
    {
        curr_byte = send(peerfd, &sendbuf[0], 1, MSG_WAITALL);
        if (curr_byte == -1)
        {
            FATAL("Failed to send RDMA QP ACK to peer :%s", strerror(errno));
        }
    } while (curr_byte != 1);
    do
    {
        curr_byte = recv(peerfd, &recvbuf[0], 1, MSG_WAITALL);
        if (curr_byte == -1)
        {
            FATAL("Failed to get RDMA QP ACK from peer :%s", strerror(errno));
        }
    } while (curr_byte != 1);
    shutdown(peerfd, SHUT_RDWR);
    //Why not do test here?
#if DEBUGON == 1
    test_rdma_recv(proc_idx);
#endif

    process_add_rdma(&rdma_processes[proc_idx].metaqueue, proc_idx);
    return true;

}

void rdma_ack_handler(metaqueue_ctl_element * req_ele, int qid)
{
    metaqueue_t * res_metaqueue(nullptr);
    int key = req_ele->req_relay_recv.shmem;
    //We need to find which process is map to the key
    if (rdma_key2qid.find(key) == rdma_key2qid.end())
        FATAL("Failed to find the shm key");
    int qid1,qid2, peerqid;
    std::tie(qid1, qid2) = rdma_key2qid[key];
    if (!process_isRDMA(qid1))
        peerqid = qid1;
    else
        peerqid = qid2;

    if (peerqid == qid)
        FATAL("Failed to find peer qid");
    //Find the metaqueue of the peerqid
    res_metaqueue = process_gethandler_byqid(peerqid);
    res_metaqueue->q[0].push(*req_ele);
    DEBUG("Monitor relay QP ACK finished!");
}


#undef DEBUGON
