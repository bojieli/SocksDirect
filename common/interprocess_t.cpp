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

short interprocess_t::buffer_t::pushdata(uint8_t *start_ptr, int size)
{
    int size_left = size;
    uint8_t *curr_ptr = start_ptr;
    short prev_blk = -1;
    short ret = -1;
    while (size_left > 0)
    {
        int blk;
        bool isAvail = avail_slots->pop_nb(blk);
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

short interprocess_t::buffer_t::popdata(unsigned short src, int &size, uint8_t *user_buf)
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
            avail_slots->push(current_loc);
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

    //is full?
    while (data->data[head & INTERPROCESS_Q_MASK].isvalid)
            SW_BARRIER;
    input.isvalid = 0;
    input.isdel = 0;
    SW_BARRIER;
    data->data[head & INTERPROCESS_Q_MASK] = input;
    SW_BARRIER;
    data->data[head & INTERPROCESS_Q_MASK].isvalid = 1;
    SW_BARRIER;
    head++;
}

void interprocess_t::queue_t::clear()
{
    memset(data->data, 0, sizeof(data->data));
}

void interprocess_t::queue_t::pop(element &output)
{
    //is empty?
    while (!data->data[tail & INTERPROCESS_Q_MASK].isvalid)
            SW_BARRIER;
    output = data->data[tail & INTERPROCESS_Q_MASK];
    SW_BARRIER;
    data->data[tail & INTERPROCESS_Q_MASK].isvalid = 0;
    SW_BARRIER;
    tail++;
    while (data->data[tail & INTERPROCESS_Q_MASK].isvalid
           && data->data[tail & INTERPROCESS_Q_MASK].isdel)
        tail++;
}

void interprocess_t::queue_t::peek(int location, element &output)
{
    output = data->data[location];
}

void interprocess_t::queue_t::del(int location)
{
    data->data[location].isdel = 1;
    if ((data->data[tail & INTERPROCESS_Q_MASK].isvalid) && (tail == location))
    {
        data->data[location].isvalid = 0;
        ++tail;
        while (data->data[tail & INTERPROCESS_Q_MASK].isdel)
        {
            data->data[tail & INTERPROCESS_Q_MASK].isvalid = 0;
            ++tail;
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
    if (loc == 0)
    {
        b_avail[0].init(memory);
        memory += locklessqueue_t<int, 2048>::getmemsize();
        b_avail[1].init((void *) memory);
        memory += locklessqueue_t<int, 2048>::getmemsize();
        q[0].init(reinterpret_cast<queue_t::data_t *>(memory));
        memory += sizeof(queue_t::data_t);
        q[1].init(reinterpret_cast<queue_t::data_t *>(memory));
        memory += sizeof(queue_t::data_t);
        b[0].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[0]);
        memory += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;
        b[1].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[1]);
    } else
    {
        b_avail[1].init(memory);
        memory += locklessqueue_t<int, 2048>::getmemsize();
        b_avail[0].init((void *) memory);
        memory += locklessqueue_t<int, 2048>::getmemsize();
        q[1].init(reinterpret_cast<queue_t::data_t *>(memory));
        memory += sizeof(queue_t::data_t);
        q[0].init(reinterpret_cast<queue_t::data_t *>(memory));
        memory += sizeof(queue_t::data_t);
        b[1].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[1]);
        memory += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;
        b[0].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[0]);

    }
    q[0].clear();
    b[0].init_mem();
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        b_avail[1].push(i);
}
