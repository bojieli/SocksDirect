//
// Created by ctyi on 11/30/17.
//

#ifndef IPC_DIRECT_INTERPROCESS_BUFFER_H
#define IPC_DIRECT_INTERPROCESS_BUFFER_H

#include <cstdint>
#include <pthread.h>
#include "locklessqueue_n.hpp"

#define INTERPROCESS_SLOTS_IN_BUFFER 1024
#define INTERPROCESS_SLOTS_IN_QUEUE 256
#define INTERPROCESS_Q_MASK ((INTERPROCESS_SLOTS_IN_QUEUE)-1)
#define INTERPROCESS_SLOTS_BLK_SIZE 1018
#define MAX_FD_NUM 102400000
#define MAX_QUEUE_NUM 100
class interprocess_t
{
public:
    class buffer_t
    {
    public:
        class element;
    private:
        element *mem;

        bool isRDMA;
        uint64_t rdma_remote_baseaddr;
        uint32_t rdma_lkey, rdma_rkey;
        ibv_qp* rdma_qp;
        ibv_cq* rdma_cq;
    public:
        class element
        {
        public:
            uint16_t size;
            uint16_t offset;
            unsigned char data[INTERPROCESS_SLOTS_BLK_SIZE];
            short next_ptr;
        };

        buffer_t();

        void init(element *_mem, locklessqueue_t<int, 2048> *_avail_slots);

        void init_mem();

        short pushdata(uint8_t *start_ptr, int size) volatile ;

        short popdata(unsigned short src, int &size, uint8_t *user_buf) volatile ;
        short popdata_nomemrelease(unsigned short src, int &size, uint8_t *user_buf) volatile ;

        void initRDMA(ibv_qp *_qp, ibv_cq* _cq, uint32_t _lkey, uint32_t _rkey,
                      uint64_t _remote_addr_mem, uint64_t _remote_addr_avail_slots);

        locklessqueue_t<int, 2048> *avail_slots;


    };

    class queue_t
    {
    public:
        class __attribute__((packed)) element
        {
        public:
            struct __attribute__((packed)) fd_notify_t
            {
                int fd;
            };
            struct __attribute__((packed)) fd_rw_t
            {
                int pointer;
                int fd;
            };
            struct __attribute__((packed)) fd_rw_zc_t
            {
                int fd;
                unsigned short num_pages;
                unsigned short page_high;
                unsigned int page_low;
            };
            struct __attribute__((packed)) fd_rw_zcv_t
            {
                int pointer;
                int fd;
                unsigned short num_pages;
            };
            struct __attribute__((packed)) zc_ret_t
            {
                unsigned long page;
                unsigned short num_pages;
            };
            struct __attribute__((packed)) zc_retv_t
            {
                int pointer;
                unsigned short num_pages;
            };
            struct __attribute__((packed)) pot_rw_t
            {
                int fd;
                uint8_t raw[9];
            };
            struct __attribute__((packed)) close_t
            {
                int req_fd;
                int peer_fd;
            };

            union __attribute__((packed))
            {
                unsigned char raw[13];
                fd_notify_t data_fd_notify;
                fd_rw_t data_fd_rw;
                fd_rw_zc_t data_fd_rw_zc;
                fd_rw_zcv_t data_fd_rw_zcv;
                zc_ret_t zc_ret;
                zc_retv_t zc_retv;
                close_t close_fd;
                pot_rw_t pot_fd_rw;
            };
            unsigned char command;
        };
    };

    locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE> q[2], q_emergency[2];
    buffer_t b[2];
    locklessqueue_t<int, 2048> b_avail[2];
    pthread_mutex_t * rd_mutex;
    bool (*sender_turn[2])[MAX_FD_NUM];
    static int get_sharedmem_size()
    {
        return (
                2 * locklessqueue_t<int, 2 * INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize() +
                2 * locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize() +
                2 * locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize() +
                2 * sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER + sizeof(pthread_mutex_t) + sizeof(bool) * MAX_FD_NUM * 2
                );
    }

    void init(key_t shmem_key, int loc);

    void init(void *baseaddr, int loc);

    void initRDMA(ibv_qp *_qp, ibv_cq* _cq, uint32_t _lkey, uint32_t _rkey,
                  uint64_t _remote_base_addr, void *baseaddr,int loc);

    static void monitor_init(void *baseaddr);

    enum cmd
    {
        NEW_FD=2,
        DATA_TRANSFER,
        DATA_TRANSFER_ZEROCOPY,
        DATA_TRANSFER_ZEROCOPY_VECTOR,
        ZEROCOPY_RETURN,
        ZEROCOPY_RETURN_VECTOR,
        NOP,
        CLOSE_FD,
        RDMA_ACK,
        RTS_RELAY_ACK,
        RTS_RELAY_DATA,
        CLOSE_REQ_NORD,
        CLOSE_REQ_NOWR,
        CLOSE_ACK_NORD,
        CLOSE_ACK_NOWR,
    };
};

#endif //IPC_DIRECT_INTERPROCESS_BUFFER_H
