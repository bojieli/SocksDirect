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
#include "../common/metaqueue.h"

#define DEBUGON 1
#define RDMA_MAX_CONN 4
#define RDMA_MAX_METAQUEUE 4


std::unordered_map<uint32_t, rdma_metaqueue > remote_monitor;


struct rdma_lib_private_info_t
{
    ibv_cq* shared_cq;
    uintptr_t local_metaqueue_base_addr;
    uintptr_t local_interprocess_base_addr;

} rdma_lib_private_info;
static rdma_pack rdma_lib_context;
static int rdma_metaqueue_buffer_seq=0;
static int rdma_interprocess_seq=0;

void rdma_init()
{
    //several things
    //1. Init a large buffer
    //2. Allocate PD
    //3. enumerate device and get dev id

    //enumerate device
    if (enum_dev(&rdma_lib_context) == -1) {
        return;
        DEBUG("WARN: RDMA NIC not present, skipping initialization");
    }
    //allocate pd
    rdma_lib_context.ibv_pd = ibv_alloc_pd(rdma_lib_context.ib_ctx);
    if (rdma_lib_context.ibv_pd == nullptr)
        FATAL("Failed to create Protected Domain for RDMA");

    //allocate a large buffer
    ssize_t  shared_buf_size=
                    (size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size() +
                    (size_t)RDMA_MAX_METAQUEUE * metaqueue_t::get_sharememsize();
    rdma_lib_context.MR_ptr = memalign(4096, shared_buf_size);
    printf("%lldbytes\n", shared_buf_size);
    rdma_lib_private_info.local_metaqueue_base_addr = (uintptr_t)rdma_lib_context.MR_ptr;
    rdma_lib_private_info.local_interprocess_base_addr = (uintptr_t)(rdma_lib_context.MR_ptr +
            (size_t)RDMA_MAX_METAQUEUE * metaqueue_t::get_sharememsize());

    if (rdma_lib_context.MR_ptr == nullptr)
        FATAL("Failed to create a large buffer %s", strerror(errno));
    //reg it to NIC MR

    int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    rdma_lib_context.buf_mr = ibv_reg_mr(rdma_lib_context.ibv_pd, rdma_lib_context.MR_ptr,
                                         shared_buf_size,ib_flags);
    DEBUG("Mem region size %u, lkey %d, rkey %d", rdma_lib_context.buf_mr->length, rdma_lib_context.buf_mr->lkey, rdma_lib_context.buf_mr->rkey);
    if (rdma_lib_context.buf_mr == nullptr)
        FATAL("Failed to reg MR for RDMA");



    //create a shared send_cq
    rdma_lib_private_info.shared_cq = ibv_create_cq(rdma_lib_context.ib_ctx, QPRQDepth+QPSQDepth, nullptr, nullptr, 0);

    if (rdma_lib_private_info.shared_cq == nullptr)
        FATAL("Failed to create a shared CQ");


    DEBUG("RDMA Init finished!");
}

rdma_self_pack_t * lib_new_qp()
{
    rdma_self_pack_t * ret = new rdma_self_pack_t;
    //Create a CQ for sender part
    ret->send_cq = ibv_create_cq(rdma_lib_context.ib_ctx, QPRQDepth+QPSQDepth, nullptr, nullptr, 0);
    ret->recv_cq = rdma_lib_private_info.shared_cq;
    ret->qp = rdma_create_qp(ret->send_cq, ret->recv_cq, &rdma_lib_context);
    ret->rkey = rdma_lib_context.buf_mr->rkey;
    ret->localptr = (uint64_t) ((uint8_t *)rdma_lib_private_info.local_interprocess_base_addr +
            interprocess_t::get_sharedmem_size() * rdma_interprocess_seq);
    memset((void*)ret->localptr, 0, interprocess_t::get_sharedmem_size());
    ++rdma_interprocess_seq;
    ret->qpn = ret->qp->qp_num;
    ret->RoCE_gid = rdma_lib_context.RoCE_gid;
    ret->rkey = rdma_lib_context.buf_mr->rkey;
    ret->buf_size =  interprocess_t::get_sharedmem_size();
    ret->port_lid = rdma_lib_context.port_lid;
    return ret;
}

rdma_pack *rdma_get_pack()
{
    return &rdma_lib_context;
}
#undef DEBUGON
#define DEBUGON 0
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
        metaqueue.qp = rdma_create_qp(rdma_lib_private_info.shared_cq,rdma_lib_private_info.shared_cq,&rdma_lib_context);

        //Now exchange QP info

        //send QP info to peer
        union {
            uint8_t sendbuf[sizeof(qp_info_t)];
            qp_info_t my_qpinfo;
        };

        my_qpinfo.qpn = metaqueue.qp->qp_num;
        my_qpinfo.RoCE_gid = rdma_lib_context.RoCE_gid;
        my_qpinfo.rkey = rdma_lib_context.buf_mr->rkey;
        my_qpinfo.remote_buf_addr = rdma_lib_private_info.local_metaqueue_base_addr +
                rdma_metaqueue_buffer_seq * metaqueue_t::get_sharememsize();
        my_qpinfo.buf_size =  metaqueue_t::get_sharememsize();
        my_qpinfo.port_lid = rdma_lib_context.port_lid;

        rdma_metaqueue_buffer_seq++;

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
        metaqueue.baseaddr = (void *)my_qpinfo.remote_buf_addr;
        metaqueue.queue.init_memlayout((uint8_t *)metaqueue.baseaddr, 1);
        metaqueue.queue.initRDMA(metaqueue.qp, rdma_lib_private_info.shared_cq,
                                 rdma_lib_context.buf_mr->lkey, metaqueue.qp_info.rkey,
        metaqueue.qp_info.remote_buf_addr, 1);
        metaqueue.queue.mem_init();
        remote_monitor[remote_addr.s_addr]=metaqueue;

        //We should block here until peer QP created

        ssize_t curr_byte;
        do
        {
            curr_byte = send(sock_fd, &sendbuf[0], 1, MSG_WAITALL);
            if (curr_byte == -1)
            {
                FATAL("Failed to send RDMA QP ACK to peer :%s", strerror(errno));
            }
        } while (curr_byte != 1);
        do
        {
            curr_byte = recv(sock_fd, &recvbuf[0], 1, MSG_WAITALL);
            if (curr_byte == -1)
            {
                FATAL("Failed to get RDMA QP ACK from peer :%s", strerror(errno));
            }
        } while (curr_byte != 1);
        shutdown(sock_fd, SHUT_RDWR);

#if DEBUGON == 1
        uint32_t cnt(0);
        while (true)
        {
            metaqueue_ctl_element ele;
            ele.command = REQ_NOP;
            ele.test_payload = ++cnt;
            metaqueue.queue.q[0].push(ele);
            //post_rdma_write(metaqueue.queue.q[0].__get_addr(),&rdma_lib_context,
            //                (uintptr_t)((uint8_t *)metaqueue.qp_info.remote_buf_addr + ((uint8_t *)metaqueue.queue.q[0].__get_addr()-(uint8_t *)metaqueue.baseaddr)),
            //metaqueue.qp_info.rkey, metaqueue.qp);
        }
#endif
    }
    return &remote_monitor[remote_addr.s_addr];
}


#undef DEBUGON
