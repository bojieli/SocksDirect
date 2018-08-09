//
// Created by ctyi on 7/17/18.
//

#ifndef IPC_DIRECT_RDMA_MONITOR_H
#define IPC_DIRECT_RDMA_MONITOR_H

#define MAX_RDMA_CONN_NUM 32

#include <infiniband/verbs.h>
#include "../common/rdma.h"
struct remote_process_t
{
    ibv_qp* myqp;
    uint32_t rkey;
    intptr_t remote_buf_ptr;
    int32_t qid;
};

void rdma_init();
#endif //IPC_DIRECT_RDMA_H
