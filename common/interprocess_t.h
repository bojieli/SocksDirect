//
// Created by ctyi on 11/30/17.
//

#ifndef IPC_DIRECT_INTERPROCESS_BUFFER_H
#define IPC_DIRECT_INTERPROCESS_BUFFER_H

#include <cstdint>
#include <pthread.h>

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

        inline void init();

        short pushdata(uint8_t *start_ptr, int size);

        short popdata(unsigned short src, int &size, uint8_t *user_buf);

    private:
        element mem[INTERPROCESS_SLOTS_IN_BUFFER];
        short avail_slots[INTERPROCESS_SLOTS_IN_BUFFER];
        int top;
    };

    class queue_t
    {
    public:
        class element
        {
        public:
            struct fd_notify_t
            {
                int fd;
            };
            struct fd_rw_t
            {
                short pointer;
                int fd;
            };

            union
            {
                unsigned char raw[13];
                fd_notify_t data_fd_notify;
                fd_rw_t data_fd_rw;
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

        void clear();

        void pop(element &data);

        void push(element &data);

        void peek(int location, element &data);

        void del(int location);

        bool isempty();
    };

    queue_t q[2];
    buffer_t *b[2];

    interprocess_t() : b{nullptr, nullptr}
    {
        b[0] = b[1] = nullptr;
    }

    static int get_sharedmem_size()
    {
        return (2 * sizeof(queue_t::data_t) + 2 * sizeof(buffer_t));
    }

    void init(key_t shmem_key, int loc);

    enum cmd
    {
        NEW_FD,
        DATA_TRANSFER,
    };
};

#endif //IPC_DIRECT_INTERPROCESS_BUFFER_H
