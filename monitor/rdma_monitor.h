//
// Created by ctyi on 7/17/18.
//

#ifndef IPC_DIRECT_RDMA_MONITOR_H
#define IPC_DIRECT_RDMA_MONITOR_H

#define RDMA_SOCK_PORT 3000
#define MAX_RDMA_CONN_NUM 32

#include <infiniband/verbs.h>
struct remote_process_t
{
    ibv_qp* myqp;
    uint32_t rkey;
    void* remote_buf_ptr;
    int32_t qid;
};

struct qp_info_t
{
    uint16_t port_lid;
    union ibv_gid RoCE_gid;
    uint32_t qpn;
    uint32_t buf_size;
    uintptr_t remote_buf_addr;
    uint32_t rkey;
    int32_t qid;
};
void rdma_init();
#endif //IPC_DIRECT_RDMA_H
