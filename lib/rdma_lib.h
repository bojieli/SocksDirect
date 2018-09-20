//
// Created by ctyi on 7/12/18.
//

#ifndef IPC_DIRECT_RDMA_LIB_H
#define IPC_DIRECT_RDMA_LIB_H

#include <assert.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "../common/helper.h"
#include "../common/rdma.h"

#include "../common/metaqueue.h"
class rdma_metaqueue
{
public:
    ibv_qp* qp;
    qp_info_t qp_info;
    void* baseaddr;
    int sender_credits;
    metaqueue_t queue;
};


struct rdma_peer_t
{
    ibv_qp* myqp;
    ibv_cq* send_cq;
    uint32_t rkey;
    intptr_t remote_buf_ptr;
    int32_t qid;
};


struct rdma_self_pack_t
{
    uint16_t port_lid;
    union ibv_gid RoCE_gid;
    uint32_t qpn;
    uint32_t buf_size;
    uint32_t rkey;
    ibv_qp *qp;
    ibv_cq *send_cq, *recv_cq;
    uint64_t localptr;
};

extern void rdma_init();
extern rdma_metaqueue * rdma_try_connect_remote_monitor(struct in_addr remote_addr);
extern rdma_self_pack_t * lib_new_qp();
rdma_pack *rdma_get_pack();




#endif //IPC_DIRECT_RDMA_H
