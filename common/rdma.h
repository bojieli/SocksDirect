//
// Created by ctyi on 7/30/18.
//

#ifndef IPC_DIRECT_COMMON_RDMA_H
#define IPC_DIRECT_COMMON_RDMA_H

#include <stdint.h>

#include <assert.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "../common/helper.h"

class rdma_pack
{
public:
    uint8_t dev_port_id;
    struct ibv_context* ib_ctx;
    uint8_t device_id;
    uint16_t port_lid;
    union ibv_gid RoCE_gid;
    struct ibv_pd * ibv_pd;
    ibv_mr* buf_mr;


    void *MR_ptr;
    uint32_t MR_rkey;

};
void enum_dev(rdma_pack *p);

static constexpr size_t QPSQDepth = 128;  ///< Depth of all SEND queues
static constexpr size_t QPRQDepth = 512;
static constexpr size_t QPMaxInlineData = 16;
#endif //IPC_DIRECT_RDMA_H
