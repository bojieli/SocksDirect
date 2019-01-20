//
// Created by ctyi on 7/30/18.
//

#ifndef IPC_DIRECT_COMMON_RDMA_H
#define IPC_DIRECT_COMMON_RDMA_H

#include <stdint.h>
#include <vector>
#include <assert.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "../common/helper.h"
#include "rdma_struct.h"



constexpr int SEND_PER_SIGNAL=64;
int enum_dev(rdma_pack *p);

//give send_cq and context, create a CQ and set to INIT state
extern ibv_qp * rdma_create_qp(ibv_cq* send_cq, ibv_cq* recv_cq, const rdma_pack * rdma_context);
extern void rdma_connect_remote_qp(ibv_qp *qp, const rdma_pack * rdma_context, const qp_info_t * remote_qp_info);
extern void post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey, ibv_qp * qp, ibv_cq * cq, size_t len);

static constexpr size_t QPSQDepth = 2560;  ///< Depth of all SEND queues
static constexpr size_t QPRQDepth = 512;
static constexpr size_t QPMaxInlineData = 16;


class RDMA_flow_ctl_t
{
public:
    class unpushed_data_t
    {
    public:
        class mem_buffer_t
        {
        public:
            bool isexist;
            void *localptr;
            uint64_t remoteptr;
            size_t size;
        };
        virtual mem_buffer_t get()=0;
    };
private:
    std::vector<unpushed_data_t *> entry_lst;
    uint32_t avail_sq_depth;
    uint32_t num_req_sent;
    uint32_t lkey, rkey;
    ibv_qp * qp;
    ibv_cq * send_cq;
private:
    void post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey,
                         ibv_qp * qp, size_t len, bool signaled);
public:
    RDMA_flow_ctl_t(uint32_t _sq_depth, uint32_t _lkey, uint32_t _rkey, ibv_qp *_qp, ibv_cq *_send_cq);
    void reg(unpushed_data_t * entry);
    void sync();
};





#endif //IPC_DIRECT_RDMA_H
