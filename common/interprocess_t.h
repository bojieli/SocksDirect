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

class interprocess_t
{
public:
    class buffer_t
    {
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

        locklessqueue_t<int, 2048> *avail_slots;

    private:
        element *mem;
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
                close_t close_fd;
            };
            unsigned char isvalid;
            unsigned char command;
            unsigned char isdel; //set to 1 when data is deleted
        };

        class data_t
        {
        public:
            element data[INTERPROCESS_SLOTS_IN_QUEUE];
        };

        data_t *data;
        union
        {
            uint8_t head;
            uint8_t tail;
        };

        queue_t() : data(nullptr), head(0)
        {}

        queue_t(data_t *_data);

        void init(data_t *data);

        void clear()  ;

        void pop(element &data);

        void push(element &data)  ;

        void peek(int location, element &data) volatile ;

        void del(int location) volatile;

        bool isempty();
    };

    queue_t q[2];
    buffer_t b[2];
    locklessqueue_t<int, 2048> b_avail[2];

    static int get_sharedmem_size()
    {
        return (2 * sizeof(queue_t::data_t) + 2 * sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER
                + 2 * locklessqueue_t<int, 2048>::getmemsize());
    }

    void init(key_t shmem_key, int loc);

    void init(void *baseaddr, int loc);

    static void monitor_init(void *baseaddr);

    enum cmd
    {
        NEW_FD=2,
        DATA_TRANSFER,
        CLOSE_FD
    };
};

#endif //IPC_DIRECT_INTERPROCESS_BUFFER_H
