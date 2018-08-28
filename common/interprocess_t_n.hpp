//
// Created by ctyi on 8/23/18.
//

#ifndef IPC_DIRECT_INTERPROCESS_T_N_HPP
#define IPC_DIRECT_INTERPROCESS_T_N_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <infiniband/verbs.h>
#include "../common/helper.h"
#include "metaqueue.h"
#include "interprocess_t.h"
constexpr int SEND_PER_SIGNAL=64;


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
    typedef interprocess_t::queue_t::element ele_t;
    class _iterator
    {
    public:
        virtual _iterator & next() = 0;
        virtual std::tuple<bool, bool> peek(ele_t &output) = 0;
        virtual void del() = 0;
        virtual void rst_offset(unsigned char command, short offset) = 0;
    };


    class iterator
    {
    public:
        explicit iterator(_iterator *_iter):iter(_iter) {}
        void destroy()
        {
            delete iter;
        }
        _iterator * iter;
        iterator & next()
        {
            iter->next();
            return *this;
        }
        std::tuple<bool, bool> peek(interprocess_t::queue_t::element &output)
        {
            return iter->peek(output);
        }
        void del()
        {
            iter->del();
        }
        void rst_offset(unsigned char command, short offset)
        {
            iter->rst_offset(command, offset);
        }

    };

    //virtual bool push_emergency_nb(const metaqueue_ctl_element &data);
    //virtual bool pop_emergency_nb(const metaqueue_ctl_element &data);
    virtual int push_data(const ele_t &in_meta, int len, void* ptr) = 0;  //This will update the len option
    /*
     * Pop data is a little tricky: It will modify the element pointed by iter. If all the data inside iter poped, iter deleted
     */
    virtual int pop_data(iterator *iter, int len, void* ptr) = 0;
    virtual size_t get_shmem_size() = 0;
    virtual iterator begin() = 0;
};

class interprocess_local_t: public interprocess_n_t, public interprocess_t
{
private:
    public:
    size_t get_shmem_size() final
    {
        return interprocess_t::get_sharedmem_size();
    }

    class _iterator: public interprocess_n_t::_iterator {
    private:
        locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE> *q;
    public:
        uint32_t pointer;
        _iterator():pointer(0),q(nullptr){}
        void init(uint32_t _pointer, locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE> *_q)
        {
            pointer = _pointer;
            q = _q;
        }

        _iterator &next() final
        {
            ++pointer;
            return *this;
        }

        std::tuple<bool, bool> peek(interprocess_t::queue_t::element &output) final
        {
            return q->peek(pointer, output);
        }

        void del()  final
        {
            q->del(pointer);
            pointer = q->pointer;
        }

        void rst_offset(unsigned char command, short offset) final
        {
            ele_t ele;
            q->peek(pointer, ele);
            switch (command)
            {
                case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR:
                    ele.zc_retv.pointer = offset;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER:
                    ele.data_fd_rw.pointer = offset;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                    ele.data_fd_rw_zcv.pointer = offset;
                    break;
                default:
                    FATAL("Unknown command for rst offset, give me some hint"
                          "about how to set the length of data in metadata");

            }
            SW_BARRIER;
            q->set(pointer, ele);
            SW_BARRIER;
        }

    };

    interprocess_n_t::iterator begin() final
    {
        _iterator *iter = new _iterator;
        iter->init(q[1].pointer, &q[1]);
        interprocess_n_t::iterator iter_ret(iter);
        return iter_ret;
    }

    int push_data(const ele_t &in_meta, int len, void* ptr) final
    {
        ele_t topush_ele=in_meta;
        if (len> 0)
        {
            short start_loc = b[0].pushdata((uint8_t *) ptr, len);
            switch (topush_ele.command)
            {
                case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR:
                    topush_ele.zc_retv.pointer = start_loc;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER:
                    topush_ele.data_fd_rw.pointer = start_loc;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                    topush_ele.data_fd_rw_zcv.pointer = start_loc;
                    break;
                default:
                    FATAL("Unknown command for rst offset, give me some hint"
                          "about how to set the length of data in metadata");

            }
        }
        q[0].push(topush_ele);
        return len;
    };

    int pop_data(iterator *iter, int len, void* ptr) final
    {
        ele_t ele;
        iter->peek(ele);
        short blk = b[1].popdata(ele.data_fd_rw.pointer, len, (uint8_t *) ptr);
        if (blk == -1)
        {
            SW_BARRIER;
            iter->del();
            SW_BARRIER;
        } else
        {
            iter->rst_offset(ele.command, blk);
        }
        return len;
    }



};

class interprocess_RDMA_t: public interprocess_n_t
{

};



#endif //IPC_DIRECT_LOCKLESSQ_V4_HPP
