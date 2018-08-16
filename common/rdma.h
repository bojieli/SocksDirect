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
#include "rdma_struct.h"




void enum_dev(rdma_pack *p);

//give cq and context, create a CQ and set to INIT state
extern ibv_qp * rdma_create_qp(ibv_cq* send_cq, ibv_cq* recv_cq, const rdma_pack * rdma_context);
extern void rdma_connect_remote_qp(ibv_qp *qp, const rdma_pack * rdma_context, const qp_info_t * remote_qp_info);
extern void post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey, ibv_qp * qp, ibv_cq * cq, size_t len);

static constexpr size_t QPSQDepth = 512;  ///< Depth of all SEND queues
static constexpr size_t QPRQDepth = 512;
static constexpr size_t QPMaxInlineData = 16;



#endif //IPC_DIRECT_RDMA_H
