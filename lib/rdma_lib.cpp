//
// Created by ctyi on 7/12/18.
//

#include <string>
#include <malloc.h>
#include "rdma_lib.h"
#include "../common/interprocess_t.h"
#include "../common/rdma.h"

#define DEBUGON 1
#define RDMA_MAX_CONN 20



static rdma_pack rdma_lib_context;

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
    rdma_lib_context.MR_ptr = memalign(4096, (size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size());
    if (rdma_lib_context.MR_ptr == nullptr)
        FATAL("Failed to create a large buffer");
    //reg it to NIC MR

    int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    rdma_lib_context.buf_mr = ibv_reg_mr(rdma_lib_context.ibv_pd, rdma_lib_context.MR_ptr,
            (size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size(),ib_flags);
    if (rdma_lib_context.buf_mr == nullptr)
        FATAL("Failed to reg MR for RDMA");

    DEBUG("RDMA Init finished!");
}

#undef DEBUGON