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
#include "locklessq_v2_rdma.hpp"
#include "locklessqueue_n.hpp"
#include "rdma.h"



class interprocess_n_t
{
public:
    typedef interprocess_t::queue_t::element ele_t;
    class _iterator
    {
    public:
        uint32_t pointer;
        virtual _iterator & next() = 0;
        virtual std::tuple<bool, bool> peek(ele_t &output) = 0;
        virtual void del() = 0;
        virtual void rst_offset(unsigned char command, short offset) = 0;
        virtual ~_iterator()
        {

        }
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
    virtual bool is_full() = 0;
    /*
     * Pop data is a little tricky: It will modify the element pointed by iter. If all the data inside iter poped, iter deleted
     */
    virtual int pop_data(iterator *iter, int len, void* ptr) = 0;
    virtual size_t get_shmem_size() = 0;
    virtual iterator begin() = 0;
    void add_remote_page_to_pool(unsigned long page) {
        // do nothing by default
    }
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
        _iterator():q(nullptr){}
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
        ele_t topush_ele = in_meta;
        short start_loc = 0; // default success
        if (len > 0)
        {
            switch (topush_ele.command)
            {
                case interprocess_t::cmd::ZEROCOPY_RETURN: {
                    DEBUG("push zero copy return");
                    break;
                }
                case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR: {
                    start_loc = b[0].pushdata((uint8_t *) ptr, len);
                    topush_ele.zc_retv.pointer = start_loc;
                    DEBUG("push zero copy return vector len %d", len);
                    break;
                }
                case interprocess_t::cmd::DATA_TRANSFER: {
                    start_loc = b[0].pushdata((uint8_t *) ptr, len);
                    topush_ele.data_fd_rw.pointer = start_loc;
                    break;
                }
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY: {
                    DEBUG("push zero copy");
                    break;
                }
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR: {
                    start_loc = b[0].pushdata((uint8_t *) ptr, len);
                    topush_ele.data_fd_rw_zcv.pointer = start_loc;
                    DEBUG("push zero copy vector len %d", len);
                    break;
                }
                default:
                    FATAL("Unknown command for rst offset, give me some hint"
                          "about how to set the length of data in metadata");

            }
        }
        // we do not have enough buffer to push data.
        // we return 0 to indicate failure, without pushing the element in queue.
        if (start_loc == -1) {
            return 0;
        }

        q[0].push(topush_ele);
        return len;
    };

    bool is_full()
    {
        return q[0].is_full();
    }

    int pop_data(iterator *iter, int len, void* ptr) final
    {
        ele_t ele;
        iter->peek(ele);

        unsigned int pointer = 0;
        switch (ele.command) {
            case interprocess_t::cmd::ZEROCOPY_RETURN:
                FATAL("unexpected pop_data from ZEROCOPY_RETURN command");
                break;
            case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR:
                pointer = ele.zc_retv.pointer;
                break;
            case interprocess_t::cmd::DATA_TRANSFER:
                pointer = ele.data_fd_rw.pointer;
                break;
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY:
                FATAL("unexpected pop_data from ZEROCOPY command");
                break;
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                pointer = ele.data_fd_rw_zcv.pointer;
                break;
            default:
                FATAL("Unknown command in pop_data");
        }

        short blk = b[1].popdata(pointer, len, (uint8_t *) ptr);
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
    typedef locklessqueue_t<interprocess_t::queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE> q_emergency_type;
    locklessq_v2_rdma q[2];
    q_emergency_type q_emergency[2];
    RDMA_flow_ctl_t *RDMA_flow;
public:

    class _iterator: public interprocess_n_t::_iterator {
    private:
        locklessq_v2_rdma *q;
    public:
        _iterator():q(nullptr){}
        void init(uint32_t _pointer, locklessq_v2_rdma *_q)
        {
            pointer = _pointer;
            q = _q;
        }

        _iterator &next() final
        {
            pointer =  q->nextptr(pointer);
            return *this;
        }

        std::tuple<bool, bool> peek(interprocess_t::queue_t::element &output) final
        {
            locklessq_v2_rdma::element_t ele={};
            ele = q->peek_meta(pointer);

            //Transform from 8byte to 16byte
            output.command = ele.command;
            switch (ele.command)
            {
                case interprocess_t::cmd::NEW_FD:
                    output.data_fd_notify.fd = ele.fd;
                    break;
                case interprocess_t::cmd::CLOSE_FD:
                    output.close_fd.req_fd = ele.inner_element.close_fd.req_fd;
                    output.close_fd.peer_fd = ele.inner_element.close_fd.peer_fd;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER:
                    output.data_fd_rw.fd = ele.fd;
                    output.data_fd_rw.pointer = ele.inner_element.data_fd_rw.offset;
                    break;
                default:
                    FATAL("Unsupported FD type from 8 byte to 16 byte");
            }
            return std::make_tuple(ele.flags & (unsigned char)LOCKLESSQ_BITMAP_ISVALID, ele.flags & (unsigned char)LOCKLESSQ_BITMAP_ISDEL);
        }

        void del()  final
        {
            q->del(pointer);
            pointer = q->pointer;
        }

        void rst_offset(unsigned char command, short offset) final
        {
            locklessq_v2_rdma::element_t ele={};
            ele = q->peek_meta(pointer);
            switch (command)
            {
                case interprocess_t::cmd::ZEROCOPY_RETURN_VECTOR:
                    ele.inner_element.zc_retv.pointer = offset;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER:
                    ele.inner_element.data_fd_rw.offset = offset;
                    break;
                case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                    ele.inner_element.data_fd_rw_zcv.pointer = offset;
                    break;
                default:
                    FATAL("Unknown command for rst offset, give me some hint"
                          "about how to set the length of data in metadata");

            }
            SW_BARRIER;
            q->set_meta(pointer, ele);
            SW_BARRIER;
        }

    };


    //0 is always the sender side
    size_t get_shmem_size() final
    {
        return (size_t)(q_emergency_type::getmemsize() * 2 + 2*locklessq_v2_rdma::getmemsize());
    }

    interprocess_n_t::iterator begin() final
    {
        _iterator *iter = new _iterator;
        iter->init(q[1].pointer, &q[1]);
        interprocess_n_t::iterator iter_ret(iter);
        return iter_ret;
    }

    int push_data(const ele_t &in_meta, int len, void* ptr) final
    {
        locklessq_v2_rdma::element_t topush_ele;

        //The first thing to do is to do the reverse: from 16byte to 8byte
        topush_ele.command = in_meta.command;
        topush_ele.flags = LOCKLESSQ_BITMAP_ISVALID;
        topush_ele.size = (uint16_t)len;
        topush_ele.fd = -1;
        switch (topush_ele.command)
        {
            case interprocess_t::cmd::NEW_FD:
                topush_ele.fd = in_meta.data_fd_notify.fd;
                break;
            case interprocess_t::cmd::CLOSE_FD:
                topush_ele.inner_element.close_fd.req_fd = in_meta.close_fd.req_fd;
                topush_ele.inner_element.close_fd.peer_fd = in_meta.close_fd.peer_fd;
                break;
            case interprocess_t::cmd::DATA_TRANSFER:
                topush_ele.fd = in_meta.data_fd_rw.fd;
                topush_ele.inner_element.data_fd_rw.offset = 0;
                break;
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY:
            {
                topush_ele.fd = in_meta.data_fd_rw_zc.fd;
                unsigned long local_page = ((unsigned long)in_meta.data_fd_rw_zc.page_high << 32) + in_meta.data_fd_rw_zc.page_low;
                unsigned int num_pages = in_meta.data_fd_rw_zc.num_pages;
                unsigned long *remote_pages = (unsigned long *) malloc(sizeof(unsigned long) * num_pages);
                int sent_pages = q[0].pushdata_zerocopy(local_page, num_pages, remote_pages);
                if (sent_pages != num_pages) {
                    ERROR("FD %d: Failed to send %d pages starting from %lx via zero copy, actually %d pages sent", topush_ele.fd, num_pages, local_page, sent_pages);
                }

                ptr = (void *) remote_pages;
                len = sizeof(unsigned long) * num_pages;
                topush_ele.size = len;
                break;
            }
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
            {
                topush_ele.fd = in_meta.data_fd_rw_zcv.fd;
                unsigned int num_pages = in_meta.data_fd_rw_zcv.num_pages;
                if (num_pages * sizeof(unsigned long) != len) {
                    FATAL("length mismatch for DATA_TRANSFER_ZEROCOPY_VECTOR");
                }
                unsigned long *remote_pages = (unsigned long *) malloc(sizeof(unsigned long) * num_pages);
                unsigned long *local_pages = (unsigned long *) ptr;
                for (unsigned int i = 0; i < num_pages; i ++) {
                    int sent_pages = q[0].pushdata_zerocopy(local_pages[i], 1, &remote_pages[i]);
                    if (sent_pages != 1) {
                        ERROR("FD %d: Failed to send page %lx via zero copy", topush_ele.fd, local_pages[0]);
                    }
                }

                ptr = (void *) remote_pages;
                topush_ele.size = len;
                break;
            }
            default:
                FATAL("Unsupported FD type from 8 byte to 16 byte");
        }

        while (!q[0].push_nb(topush_ele,ptr));

        switch (topush_ele.command)
        {
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY:
            case interprocess_t::cmd::DATA_TRANSFER_ZEROCOPY_VECTOR:
                if (topush_ele.size > 0 && ptr != NULL)
                    free(ptr);
                break;
        }
        return len;
    };

    void add_remote_page_to_pool(unsigned long page) {
        q[0].add_remote_page_to_pool(page);
    }

    int pop_data(iterator *iter, int len, void* ptr) final
    {
/*
        ele_t ele={};
        iter->peek(ele);
        void * start_ptr = q[1].peek_data(iter->iter->pointer);
        return len;
        */
    }



};



#endif //IPC_DIRECT_LOCKLESSQ_V4_HPP
