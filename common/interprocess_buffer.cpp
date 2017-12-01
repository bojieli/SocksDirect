//
// Created by ctyi on 11/30/17.
//

#include <cstring>
#include <cstdlib>
#include <sys/shm.h>
#include "interprocess_buffer.h"
#include "../common/helper.h"

interprocess_buffer::buffer_t::buffer_t()  {
    for (unsigned short i=0;i<INTERPROCESS_SLOTS_IN_BUFFER;++i)
        avail_slots[INTERPROCESS_SLOTS_IN_BUFFER-i-1]=i;
    top=INTERPROCESS_SLOTS_IN_BUFFER-1;
}
void interprocess_buffer::buffer_t::init()  {
    for (unsigned short i=0;i<INTERPROCESS_SLOTS_IN_BUFFER;++i)
        avail_slots[INTERPROCESS_SLOTS_IN_BUFFER-i-1]=i;
    top=INTERPROCESS_SLOTS_IN_BUFFER-1;
}

short interprocess_buffer::buffer_t::pushdata(uint8_t *start_ptr, int size)
{
    int size_left=size;
    uint8_t *curr_ptr=start_ptr;
    short prev_blk=-1;
    while (size_left > 0)
    {
        if (top < 0) return -1;
        short blk = avail_slots[top--];
        if (prev_blk != -1) mem[prev_blk].next_ptr = blk;
        mem[blk].size = size_left;
        int true_size=size_left<sizeof(interprocess_buffer::buffer_t::element::data)?
                      size_left:sizeof(interprocess_buffer::buffer_t::element::data);
        memcpy(mem[blk].data, curr_ptr, true_size);
        curr_ptr += true_size;
        size_left -= true_size;
        if (size_left == 0) mem[blk].next_ptr = -1;
        prev_blk = blk;
    }
}

uint8_t * interprocess_buffer::buffer_t::pickdata(unsigned short src, int &size)
{
    size=mem[src].size;
    uint8_t* result = (uint8_t *)malloc(size);
    uint8_t* current_ptr = result;
    for (short ptr=src;ptr!=-1;ptr=mem[ptr].next_ptr)
    {
        int true_size= mem[ptr].size<sizeof(interprocess_buffer::buffer_t::element::data)?
        mem[ptr].size:sizeof(interprocess_buffer::buffer_t::element::data);

        memcpy(current_ptr, mem[ptr].data, true_size);
        current_ptr += true_size;
    }
    return result;
}

void interprocess_buffer::buffer_t::free(unsigned short src)
{
    for (short ptr=src;ptr!=-1;ptr=mem[ptr].next_ptr)
        avail_slots[++top]=ptr;
    mem[src].next_ptr = -1;
}

interprocess_buffer::queue_t::queue_t(data_t* _data):pointer(0)
{
    data = _data;
}

void interprocess_buffer::queue_t::init(data_t *_data)
{
    data= _data;
    pointer = 0;
}

void interprocess_buffer::queue_t::push(element &input) {

    //is full?
    while (data->data[pointer & INTERPROCESS_Q_MASK].isvalid)
        SW_BARRIER;
    input.isvalid=0;
    SW_BARRIER;
    data->data[pointer & INTERPROCESS_Q_MASK] = input;
    SW_BARRIER;
    data->data[pointer & INTERPROCESS_Q_MASK].isvalid = 1;
    SW_BARRIER;
    pointer++;
}

void interprocess_buffer::queue_t::pop(element &output) {
    //is empty?
    while (!data->data[pointer & INTERPROCESS_Q_MASK].isvalid)
            SW_BARRIER;
    output = data->data[pointer & INTERPROCESS_Q_MASK];
    SW_BARRIER;
    data->data[pointer & INTERPROCESS_Q_MASK].isvalid = 0;
    SW_BARRIER;
    pointer++;
}

void interprocess_buffer::queue_t::pick(int location, element &output)
{
    output = data->data[location];
}

void interprocess_buffer::queue_t::del(int location)
{
    data->data[location].isvalid = 0;
    if (location == pointer) pointer++;
}

bool interprocess_buffer::queue_t::isempty()
{
    for (int pointer=0;pointer<INTERPROCESS_SLOTS_IN_BUFFER;++pointer)
        if (data->data[pointer].isvalid)
            return false;
    return true;
}
void interprocess_buffer::init(key_t shmem_key, int loc)
{
    int mem_id;
    mem_id = shmget(shmem_key, 2*sizeof(get_sharedmem_size()), 0777);
    //printf("%d\n", uniq_shared_id);
    if (mem_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    void* baseaddr;
    baseaddr = shmat(mem_id, NULL, 0);
    if (baseaddr == (void*)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    if (loc == 0)
    {
        q[0].init(reinterpret_cast<queue_t::data_t *>(baseaddr));
        q[1].init(reinterpret_cast<queue_t::data_t *>(baseaddr)+1);
        b[0] = reinterpret_cast<buffer_t *>((uint8_t *)baseaddr+2*sizeof(queue_t::data_t));
        b[1] = reinterpret_cast<buffer_t *>((uint8_t *)baseaddr+2*sizeof(queue_t::data_t))+1;
    } else
    {
        q[1].init(reinterpret_cast<queue_t::data_t *>(baseaddr));
        q[0].init(reinterpret_cast<queue_t::data_t *>(baseaddr)+1);
        b[1] = reinterpret_cast<buffer_t *>((uint8_t *)baseaddr+2*sizeof(queue_t::data_t));
        b[0] = reinterpret_cast<buffer_t *>((uint8_t *)baseaddr+2*sizeof(queue_t::data_t))+1;
    }
    b[0]->init();
}