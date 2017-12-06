//
// Created by ctyi on 11/30/17.
//

#include <cstring>
#include <cstdlib>
#include <sys/shm.h>
#include "interprocess_t.h"
#include "../common/helper.h"

interprocess_t::buffer_t::buffer_t()  {
    for (unsigned short i=0;i<INTERPROCESS_SLOTS_IN_BUFFER;++i)
        avail_slots[INTERPROCESS_SLOTS_IN_BUFFER-i-1]=i;
    top=INTERPROCESS_SLOTS_IN_BUFFER-1;
}
void interprocess_t::buffer_t::init()  {
    for (unsigned short i=0;i<INTERPROCESS_SLOTS_IN_BUFFER;++i)
        avail_slots[INTERPROCESS_SLOTS_IN_BUFFER-i-1]=i;
    top=INTERPROCESS_SLOTS_IN_BUFFER-1;
}

short interprocess_t::buffer_t::pushdata(uint8_t *start_ptr, int size)
{
    int size_left=size;
    uint8_t *curr_ptr=start_ptr;
    short prev_blk=-1;
    while (size_left > 0)
    {
        if (top < 0) return -1;
        short blk = avail_slots[top--];
        if (prev_blk != -1) mem[prev_blk].next_ptr = blk;
        mem[blk].offset = 0;
        int true_size=size_left<sizeof(interprocess_t::buffer_t::element::data)?
                      size_left:sizeof(interprocess_t::buffer_t::element::data);
        memcpy(mem[blk].data, curr_ptr, true_size);
        mem[blk].size = true_size;
        curr_ptr += true_size;
        size_left -= true_size;
        if (size_left == 0) mem[blk].next_ptr = -1;
        prev_blk = blk;
    }
}

short interprocess_t::buffer_t::popdata(unsigned short src, int &size, uint8_t *user_buf)
{
    short current_loc = src;
    int sizeleft=size;
    uint8_t *curr_ptr=user_buf;
    while ((current_loc != -1) && (sizeleft > 0))
    {
        uint16_t size_inblk=mem[current_loc].size;
        uint16_t offset_inblk=mem[current_loc].offset;
        bool isfullblk= (sizeleft >= mem[current_loc].size);
        short copy_size=isfullblk?size_inblk:sizeleft;
        mempcpy(curr_ptr,&mem[current_loc].data[offset_inblk], copy_size);
        curr_ptr += copy_size;
        sizeleft -= copy_size;
        if (isfullblk)
        {
            ++top;
            avail_slots[top]=current_loc;
        } else { //last block and not fully copied
            size_inblk -= copy_size;
            offset_inblk += copy_size;
        }
        mem[current_loc].size = size_inblk;
        mem[current_loc].offset = offset_inblk;
        current_loc = mem[current_loc].next_ptr;
    }

    size-=sizeleft;
    return current_loc;
}

interprocess_t::queue_t::queue_t(data_t* _data):head(0)
{
    data = _data;
}

void interprocess_t::queue_t::init(data_t *_data)
{
    data= _data;
    head = 0;
}

void interprocess_t::queue_t::push(element &input) {

    //is full?
    while (data->data[head & INTERPROCESS_Q_MASK].isvalid)
        SW_BARRIER;
    input.isvalid=0;
    input.isdel=0;
    SW_BARRIER;
    data->data[head & INTERPROCESS_Q_MASK] = input;
    SW_BARRIER;
    data->data[head & INTERPROCESS_Q_MASK].isvalid = 1;
    SW_BARRIER;
    head++;
}

void interprocess_t::queue_t::pop(element &output) {
    //is empty?
    while (!data->data[tail & INTERPROCESS_Q_MASK].isvalid)
            SW_BARRIER;
    output = data->data[tail & INTERPROCESS_Q_MASK];
    SW_BARRIER;
    data->data[tail & INTERPROCESS_Q_MASK].isvalid = 0;
    SW_BARRIER;
    tail++;
}

void interprocess_t::queue_t::peek(int location, element &output)
{
    output = data->data[location];
}

void interprocess_t::queue_t::del(int location)
{
    data->data[location].isdel = 1;
    if ((data->data[tail & INTERPROCESS_Q_MASK].isvalid) && (tail == location)) ++tail;
    while (data->data[tail & INTERPROCESS_Q_MASK].isdel) ++tail;
}

bool interprocess_t::queue_t::isempty() {
    uint8_t tmp_tail = tail;
    bool isempty = true;
    while (data->data[tmp_tail & INTERPROCESS_Q_MASK].isvalid){
        if (!data->data[tmp_tail & INTERPROCESS_Q_MASK].isdel) {
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