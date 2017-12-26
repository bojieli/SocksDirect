//
// Created by ctyi on 11/30/17.
//

#include <cstring>
#include <cstdlib>
#include <sys/shm.h>
#include "interprocess_t.h"
#include "../common/helper.h"
#include "locklessqueue_n.hpp"

interprocess_t::buffer_t::buffer_t() : mem(nullptr), avail_slots(nullptr)
{
}

void interprocess_t::buffer_t::init(element *_mem, locklessqueue_t<int, 2048> *_avail_slots)
{
    mem = _mem;
    avail_slots = _avail_slots;

}

void interprocess_t::buffer_t::init_mem()
{
    avail_slots->init_mem();
}

short interprocess_t::buffer_t::pushdata(uint8_t *start_ptr, int size) volatile
{
    int size_left = size;
    uint8_t *curr_ptr = start_ptr;
    short prev_blk = -1;
    short ret = -1;
    while (size_left > 0)
    {
        int blk;
        bool isAvail = avail_slots->pop_nb(blk);
        SW_BARRIER;
        if (!isAvail)
        {
            if (prev_blk != -1)
                popdata(prev_blk, size, nullptr);
            return -1;
        }
        if (prev_blk != -1)
            mem[prev_blk].next_ptr = blk;
        mem[blk].offset = 0;
        int true_size = size_left < sizeof(interprocess_t::buffer_t::element::data) ?
                        size_left : sizeof(interprocess_t::buffer_t::element::data);
        memcpy(mem[blk].data, curr_ptr, true_size);
        mem[blk].size = true_size;
        curr_ptr += true_size;
        size_left -= true_size;
        if (size_left == 0) mem[blk].next_ptr = -1;
        if (prev_blk == -1) ret = blk;
        prev_blk = blk;
    }
    return ret;
}

short interprocess_t::buffer_t::popdata(unsigned short src, int &size, uint8_t *user_buf) volatile
{
    short current_loc = src;
    int sizeleft = size;
    uint8_t *curr_ptr = user_buf;
    while ((current_loc != -1) && (sizeleft != 0))
    {
        uint16_t size_inblk = mem[current_loc].size;
        uint16_t offset_inblk = mem[current_loc].offset;
        bool isfullblk = (sizeleft >= mem[current_loc].size);
        short copy_size = isfullblk ? size_inblk : sizeleft;
        if (user_buf != nullptr)
            mempcpy(curr_ptr, &mem[current_loc].data[offset_inblk], copy_size);
        curr_ptr += copy_size;
        sizeleft -= copy_size;
        if (!isfullblk)
        { //last block and not fully copied
            size_inblk -= copy_size;
            offset_inblk += copy_size;
        }
        mem[current_loc].size = size_inblk;
        mem[current_loc].offset = offset_inblk;
        if ((sizeleft == 0) && (!isfullblk)) break;
        short next_ptr = mem[current_loc].next_ptr;
        if (isfullblk)
        {
            SW_BARRIER;
            avail_slots->push(current_loc);
            SW_BARRIER;
        }
        current_loc = next_ptr;
    }

    size -= sizeleft;
    return current_loc;
}

interprocess_t::queue_t::queue_t(data_t *_data) : head(0)
{
    data = _data;
}

void interprocess_t::queue_t::init(data_t *_data)
{
    data = _data;
    head = 0;
}

void interprocess_t::queue_t::push(element &input) 
{
    element *head_ptr = &data->data[head & INTERPROCESS_Q_MASK];
    //is full?
    while (head_ptr->isvalid)
            SW_BARRIER;
    head_ptr->isvalid = 0;
    SW_BARRIER;
    input.isvalid = 0;
    input.isdel = 0;
    *head_ptr = input;
    SW_BARRIER;
    head_ptr->isvalid = 1;
    SW_BARRIER;
    head++;
}

void interprocess_t::queue_t::clear() 
{
    memset(static_cast<void *>(data->data), 0, sizeof(data->data));
}

void interprocess_t::queue_t::pop(element &output)
{
    element *tail_ptr = &data->data[tail & INTERPROCESS_Q_MASK];
    //is empty?
    while (!tail_ptr->isvalid)
            SW_BARRIER;
    output = *tail_ptr;
    SW_BARRIER;
    tail_ptr->isvalid = 0;
    SW_BARRIER;
    tail++;
    while (data->data[tail & INTERPROCESS_Q_MASK].isvalid
           && data->data[tail & INTERPROCESS_Q_MASK].isdel)
    {
        SW_BARRIER;
        tail++;
    }
}

void interprocess_t::queue_t::peek(int location, element &output) volatile
{
    SW_BARRIER;
    output = data->data[location];
    SW_BARRIER;
}

void interprocess_t::queue_t::del(int location) volatile
{
    location = location & INTERPROCESS_Q_MASK;
    data->data[location].isdel = 1;
    SW_BARRIER;
    if ((data->data[tail & INTERPROCESS_Q_MASK].isvalid) && (tail == location))
    {
        data->data[location].isvalid = 0;
        ++tail;
        SW_BARRIER;
        while (data->data[tail & INTERPROCESS_Q_MASK].isvalid
            && data->data[tail & INTERPROCESS_Q_MASK].isdel)
        {
            data->data[tail & INTERPROCESS_Q_MASK].isvalid = 0;
            ++tail;
            SW_BARRIER;
        }
    }
}

bool interprocess_t::queue_t::isempty()
{
    uint8_t tmp_tail = tail;
    bool isempty = true;
    while (data->data[tmp_tail & INTERPROCESS_Q_MASK].isvalid)
    {
        if (!data->data[tmp_tail & INTERPROCESS_Q_MASK].isdel)
        {
            isempty = false;
            break;
        }
        tmp_tail++;
    }
    return isempty;
}

void interprocess_t::init(key_t shmem_key, int loc)
{
    int mem_id;
    mem_id = shmget(shmem_key, 2 * sizeof(get_sharedmem_size()), 0777);
    //printf("%d\n", uniq_shared_id);
    if (mem_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    void *baseaddr;
    baseaddr = shmat(mem_id, NULL, 0);
    if (baseaddr == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    init(baseaddr, loc);
}

void interprocess_t::init(void *baseaddr, int loc)
{
    uint8_t *memory = (uint8_t *) baseaddr;
    int my_loc = loc;
    int peer_loc = 1 - my_loc;
    b_avail[my_loc].init(memory);
    memory += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();
    b_avail[peer_loc].init((void *) memory);
    b_avail[1].setpointer(INTERPROCESS_SLOTS_IN_BUFFER);
    b_avail[0].setpointer(0);
    memory += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();
    q[my_loc].init(reinterpret_cast<queue_t::data_t *>(memory));
    memory += sizeof(queue_t::data_t);
    q[peer_loc].init(reinterpret_cast<queue_t::data_t *>(memory));
    memory += sizeof(queue_t::data_t);
    b[my_loc].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[my_loc]);
    memory += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;
    b[peer_loc].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[peer_loc]);
}

void interprocess_t::monitor_init(void *baseaddr) {
    interprocess_t tmp;
    memset(baseaddr, 0, get_sharedmem_size());
    tmp.init(baseaddr, 0);
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        tmp.b_avail[0].push(i);
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        tmp.b_avail[1].push(i);
}

