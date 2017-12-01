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

class interprocess_buffer
{
public:
    class buffer_t {
    public:
        class element {
        public:
            int size;
            unsigned char data[1018];
            short next_ptr;
        };

        buffer_t();
        inline void init();
        short pushdata(uint8_t *start_ptr, int size);
        uint8_t * pickdata(unsigned short src, int &size);
        void free(unsigned short src);
    private:
        element mem[INTERPROCESS_SLOTS_IN_BUFFER];
        short avail_slots[INTERPROCESS_SLOTS_IN_BUFFER];
        int top;
    };
    class queue_t{
    public:
        class element{
        public:
            struct fd_notify_t{
                int fd;
            } ;
            struct fd_rw_t{
                short pointer;
                int fd;
            } ;
            unsigned short isvalid;
            unsigned short command;
            union {
                unsigned char raw[12];
                fd_notify_t data_fd_notify;
                fd_rw_t data_fd_rw;
            };
        };
        class data_t
        {
        public:
            element data[INTERPROCESS_SLOTS_IN_QUEUE];
        };
        data_t *data;
        unsigned char pointer;
        queue_t(): data(nullptr), pointer(0){}
        queue_t(data_t *_data);
        void init(data_t *data);
        void pop(element & data);
        void push(element & data);
        void pick(int location, element &data);
        void del(int location);
        bool isempty();
    };
    queue_t q[2];
    buffer_t* b[2];
    interprocess_buffer() : b{nullptr, nullptr}
    {
        b[0] = b[1] = nullptr;
    }
    static int get_sharedmem_size()
    {
        return (2*sizeof(queue_t::data_t) + 2*sizeof(buffer_t));
    }
    void init(key_t shmem_key, int loc);
};
#endif //IPC_DIRECT_INTERPROCESS_BUFFER_H
