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

static int rdma_sock_fd;
static rdma_pack rdma_monitor_context;


void create_rdma_socket()
{
    rdma_sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    create_rdma_socket();
    DEBUG("RDMA Init finished!");
}

#undef DEBUGON