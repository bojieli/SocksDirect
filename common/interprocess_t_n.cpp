//
// Created by ctyi on 8/23/18.
//

#ifndef IPC_DIRECT_LOCKLESSQ_V4_HPP
#define IPC_DIRECT_LOCKLESSQ_V4_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <infiniband/verbs.h>
#include "../common/helper.h"
#include "metaqueue.h"

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
        virtual mem_buffer_t get();
    };
private:
    std::vector<unpushed_data_t *> entry_lst;
    uint32_t avail_sq_depth;
    uint32_t num_req_sent;
    uint32_t lkey, rkey;
    ibv_qp * qp;
    ibv_cq * send_cq;
    constexpr int SEND_PER_SIGNAL=64;
private:
    void post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey,
                         ibv_qp * qp, size_t len, bool signaled)
    {

        struct ibv_send_wr wr, *bad_send_wr;
        struct ibv_sge sgl;

        int32_t ret;

        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.num_sge = 1;
        wr.next = nullptr;
        wr.sg_list = &sgl;

        wr.send_flags = 0;


        sgl.addr = (uint64_t) ele;
        sgl.length = len;
        sgl.lkey = lkey;


        wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
        wr.wr.rdma.rkey = rkey;

        /*
         * Caveat: You must regularly request CQE, otherwise CPU
         * won't know whether WQE finished, finally lead to ENOMEM because
         * Send Queue used up.
         */
        if (signaled)
            wr.send_flags |= IBV_SEND_SIGNALED;


        ret = ibv_post_send(qp, &wr, &bad_send_wr);
        if (ret != 0) {
            FATAL("wrong ret %d", ret);
            return;
        }
    }
public:
    RDMA_flow_ctl_t(uint32_t _sq_depth, uint32_t _lkey, uint32_t _rkey, ibv_qp *_qp, ibv_cq *_send_cq):
            entry_lst(),
            avail_sq_depth(_sq_depth),
            num_req_sent(0),
            lkey(_lkey),
            rkey(_rkey),
            qp(_qp),
            send_cq(_send_cq)
    {}
    void reg(unpushed_data_t * entry)
    {
        entry_lst.push_back(entry);
    }
    void sync()
    {
        int32_t cq_ret;
        do
        {
            struct ibv_wc wc;
            cq_ret = ibv_poll_cq(send_cq, 1, &wc);
            avail_sq_depth += SEND_PER_SIGNAL;
            if (cq_ret < 0)
                FATAL("Poll CQ Err %s", strerror(cq_ret));
        } while (cq_ret > 0);


        unpushed_data_t::mem_buffer_t ret;
        for (auto entry : entry_lst)
        {
            if (avail_sq_depth == 0) break;
            do {
                ret = entry->get();
                if (ret.isexist) {
                    ++num_req_sent;
                    bool issignal(false);
                    if (num_req_sent % SEND_PER_SIGNAL)
                        issignal = true;
                    post_rdma_write(ret.localptr,ret.remoteptr, lkey, rkey, qp, ret.size, issignal);
                }
            } while (ret.isexist);
        }
    }
};

class interprocess_n_t
{
public:
    virtual bool push_emergency_nb(const metaqueue_ctl_element &data);
    virtual bool pop_emergency_nb(const metaqueue_ctl_element &data);

};



#endif //IPC_DIRECT_LOCKLESSQ_V4_HPP
