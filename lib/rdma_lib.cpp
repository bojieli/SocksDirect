//
// Created by ctyi on 7/12/18.
//

#include <string>
#include <malloc.h>
#include <netinet/in.h>
#include <unordered_map>
#include <sys/un.h>
#include "rdma_lib.h"
#include "../common/interprocess_t.h"
#include "../common/rdma.h"

#define DEBUGON 1
#define RDMA_MAX_CONN 20
#define RDMA_MAX_METAQUEUE 20


std::unordered_map<uint32_t, rdma_metaqueue > remote_monitor;


struct rdma_lib_private_info_t
{
    ibv_cq* shared_cq;
    uintptr_t local_metaqueue_base_addr;
    uintptr_t local_interprocess_base_addr;

} rdma_lib_private_info;
static rdma_pack rdma_lib_context;
static int rdma_buffer_seq=0;

void rdma_init()
{
    //several things
    //1. Init a large buffer
    //2. Allocate PD
    //3. enumerate device and get dev id

    //enumerate device
    enum_dev(&rdma_lib_context);
    //allocate pd
    rdma_lib_context.ibv_pd = ibv_alloc_pd(rdma_lib_context.ib_ctx);
    if (rdma_lib_context.ibv_pd == nullptr)
        FATAL("Failed to create Protected Domain for RDMA");

    //allocate a large buffer
    ssize_t  shared_buf_size=
                    (size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size() +
                    (size_t)RDMA_MAX_METAQUEUE * metaqueue_t::get_sharememsize();
    rdma_lib_context.MR_ptr = memalign(4096, shared_buf_size);
    rdma_lib_private_info.local_metaqueue_base_addr = (uintptr_t)rdma_lib_context.MR_ptr;
    rdma_lib_private_info.local_interprocess_base_addr = (uintptr_t)(rdma_lib_context.MR_ptr +
            (size_t)RDMA_MAX_METAQUEUE * metaqueue_t::get_sharememsize());

    if (rdma_lib_context.MR_ptr == nullptr)
        FATAL("Failed to create a large buffer");
    //reg it to NIC MR

    int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    rdma_lib_context.buf_mr = ibv_reg_mr(rdma_lib_context.ibv_pd, rdma_lib_context.MR_ptr,
                                         shared_buf_size,ib_flags);
    if (rdma_lib_context.buf_mr == nullptr)
        FATAL("Failed to reg MR for RDMA");



    //create a shared cq
    rdma_lib_private_info.shared_cq = ibv_create_cq(rdma_lib_context.ib_ctx, QPRQDepth+QPSQDepth, nullptr, nullptr, 0);

    if (rdma_lib_private_info.shared_cq == nullptr)
        FATAL("Failed to create a shared CQ");


    DEBUG("RDMA Init finished!");
}

rdma_metaqueue * rdma_try_connect_remote_monitor(struct in_addr remote_addr)
{
    //First check whether existing connection exist to the remote monitor
    if (remote_monitor.find(remote_addr.s_addr) == remote_monitor.end())
    {
        // No RDMA connection to monitor
        rdma_metaqueue metaqueue;
        /*
         * 1. First TCP connection
         * 2. If success, create QP, create buffer
         * 3. send & exchange QP info
         * 4. Connect QP
         */
        int sock_fd;
        sock_fd = ORIG(socket, (AF_INET, SOCK_STREAM, 0));
        if (sock_fd == -1)
            FATAL("Failted to create client socket, %s", strerror(errno));
        struct sockaddr_in monitor_addr;
        memset(&monitor_addr, 0, sizeof(monitor_addr));
        monitor_addr.sin_family = AF_INET;
        monitor_addr.sin_port = htons(RDMA_SOCK_PORT);
        monitor_addr.sin_addr = remote_addr;
        if (ORIG(connect, (sock_fd, (struct sockaddr *)&monitor_addr, sizeof(struct sockaddr_in))) < 0)
        {
            FATAL("Failed to create a TCP connection to remote monitor, %s", strerror(errno));
        }

        //Now try to Create QP
        metaqueue.qp = rdma_create_qp(rdma_lib_private_info.shared_cq,&rdma_lib_context);

        //Now exchange QP info

        //send QP info to peer
        union {
            uint8_t sendbuf[sizeof(qp_info_t)];
            qp_info_t my_qpinfo;
        };

        my_qpinfo.qpn = metaqueue.qp->qp_num;
        my_qpinfo.RoCE_gid = rdma_lib_context.RoCE_gid;
        my_qpinfo.rkey = rdma_lib_context.MR_rkey;
        my_qpinfo.remote_buf_addr = rdma_lib_private_info.local_metaqueue_base_addr +
                rdma_buffer_seq * metaqueue_t::get_sharememsize();
        my_qpinfo.buf_size =  metaqueue_t::get_sharememsize();
        my_qpinfo.port_lid = rdma_lib_context.port_lid;

        rdma_buffer_seq++;

        ssize_t left_byte=sizeof(qp_info_t);
        int currptr=0;
        while (left_byte > 0)
        {
            ssize_t curr_sent_byte = send(sock_fd, &sendbuf[currptr], left_byte, MSG_WAITALL);
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
            ssize_t  curr_recv_byte = recv(sock_fd, &recvbuf[currptr], left_byte, MSG_WAITALL);
            currptr += curr_recv_byte;
            left_byte -= curr_recv_byte;
        }

        rdma_connect_remote_qp(metaqueue.qp,&rdma_lib_context,&peer_qpinfo);

        //Store the addr and rkey
        metaqueue.qp_info.rkey = peer_qpinfo.rkey;
        metaqueue.qp_info.remote_buf_addr = peer_qpinfo.remote_buf_addr;


    }
    return nullptr;
}

#undef DEBUGON