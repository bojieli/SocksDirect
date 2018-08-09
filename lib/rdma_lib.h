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
};
extern void rdma_init();
extern rdma_metaqueue * rdma_try_connect_remote_monitor(struct in_addr remote_addr);




#endif //IPC_DIRECT_RDMA_H
