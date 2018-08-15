//
// Created by ctyi on 8/14/18.
//

#ifndef IPC_DIRECT_RDMA_STRUCT_H
#define IPC_DIRECT_RDMA_STRUCT_H
#include <infiniband/verbs.h>
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
#endif //IPC_DIRECT_RDMA_STRUCT_H
