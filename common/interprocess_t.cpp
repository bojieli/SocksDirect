//
// Created by ctyi on 11/30/17.
//

#include <cstring>
#include <cstdlib>
#include <sys/shm.h>
#include "interprocess_t.h"
#include "../common/helper.h"
#include "locklessqueue_n.hpp"

interprocess_t::buffer_t::buffer_t() : mem(nullptr), avail_slots(nullptr), isRDMA(false)
{
}

void interprocess_t::buffer_t::init(element *_mem, locklessqueue_t<int, 2 * INTERPROCESS_SLOTS_IN_BUFFER> *_avail_slots)
{
    mem = _mem;
    avail_slots = _avail_slots;
    isRDMA = false;

}

void interprocess_t::buffer_t::init_mem()
{
    avail_slots->init_mem();
}

inline void interprocess_t::buffer_t::initRDMA(ibv_qp *_qp, ibv_cq* _cq, uint32_t _lkey, uint32_t _rkey,
                                               uint64_t _remote_addr_mem)
{
    isRDMA = true;
    rdma_lkey = _lkey;
    rdma_rkey = _rkey;
    rdma_remote_baseaddr = _remote_addr_mem;
    rdma_qp = _qp;
    rdma_cq = _cq;
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
            /*
            if (prev_blk != -1)
                popdata(prev_blk, size, nullptr);
            return -1;*/
            return ret;

        }
        if (prev_blk != -1)
        {
            mem[prev_blk].next_ptr = blk;
            //Do some RDMA write
            if (isRDMA) {
                post_rdma_write((void *) &mem[prev_blk],
                                rdma_remote_baseaddr + ((uint8_t *) &mem[prev_blk] - (uint8_t *) mem),
                                rdma_lkey,
                                rdma_rkey,
                                rdma_qp,
                                rdma_cq,
                                sizeof(element)
                );
            }
        }
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

    //Write the last block to remote
    if (isRDMA && (prev_blk != -1))
    {
        post_rdma_write((void *) &mem[prev_blk],
                        rdma_remote_baseaddr + ((uint8_t *) &mem[prev_blk] - (uint8_t *) mem),
                        rdma_lkey,
                        rdma_rkey,
                        rdma_qp,
                        rdma_cq,
                        sizeof(element)
        );
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

short interprocess_t::buffer_t::popdata_nomemrelease(unsigned short src, int &size, uint8_t *user_buf) volatile
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
        current_loc = next_ptr;
    }

    size -= sizeleft;
    return current_loc;
}


void interprocess_t::init(key_t shmem_key, int loc)
{
    int mem_id;
    mem_id = shmget(shmem_key, sizeof(get_sharedmem_size()), 0777);
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
    b_avail[my_loc].init(memory, peer_loc);
    memory += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();
    b_avail[peer_loc].init((void *) memory, my_loc);
    b_avail[1].setpointer(INTERPROCESS_SLOTS_IN_BUFFER);
    b_avail[0].setpointer(0);
    b_avail[1].disable_credit();
    b_avail[0].disable_credit();
    memory += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();

    q[my_loc].init(memory, my_loc);
    memory += locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize();
    q[peer_loc].init(memory, peer_loc);
    memory += locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize();

    b[my_loc].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[my_loc]);
    memory += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;
    b[peer_loc].init(reinterpret_cast<buffer_t::element *>(memory), &b_avail[peer_loc]);
    memory += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;

    q_emergency[my_loc].init(memory, my_loc);
    memory += locklessqueue_t<queue_t::element, 256>::getmemsize();
    q_emergency[peer_loc].init(memory, peer_loc);
    memory += locklessqueue_t<queue_t::element, 256>::getmemsize();
    
    rd_mutex = (pthread_mutex_t *)memory;
    memory += sizeof(pthread_mutex_t);
    sender_turn[my_loc] = (bool(*) [MAX_FD_NUM])memory;
    memory += sizeof(bool) * MAX_FD_NUM;
    sender_turn[peer_loc] = (bool(*) [MAX_FD_NUM])memory;
}

void interprocess_t::monitor_init(void *baseaddr) {
    interprocess_t tmp;
    memset(baseaddr, 0, get_sharedmem_size());
    tmp.init(baseaddr, 0);
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        tmp.b_avail[0].push(i);
    tmp.init(baseaddr, 1);
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        tmp.b_avail[0].push(i);
    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(tmp.rd_mutex, &mutexattr) != 0)
        FATAL("Error to init mutex");
    
}


//This function need to be called after the init of interprocess_t
void interprocess_t::initRDMA(ibv_qp *_qp, ibv_cq* _cq, uint32_t _lkey, uint32_t _rkey,
              uint64_t _remote_base_addr, void *baseaddr, int loc)
{
    uint64_t remoteaddr(_remote_base_addr);
    int myloc = loc;
    int peer_loc = 1 - loc;

    //memset(baseaddr, 0, get_sharedmem_size());

    b_avail[1].setpointer(0);
    b_avail[0].setpointer(0);
    b_avail[0].enable_credit();
    b_avail[1].enable_credit();

    uint64_t my_b_avail_remote_addr=remoteaddr;
    b_avail[myloc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();

    uint64_t peer_b_avail_remote_addr=remoteaddr;
    b_avail[peer_loc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    for (unsigned short i = 0; i < INTERPROCESS_SLOTS_IN_BUFFER; ++i)
        b_avail[1].push(i);
    remoteaddr += locklessqueue_t<int, 2*INTERPROCESS_SLOTS_IN_BUFFER>::getmemsize();

    q[myloc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize();

    q[peer_loc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += locklessqueue_t<queue_t::element, INTERPROCESS_SLOTS_IN_QUEUE>::getmemsize();

    b[myloc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;
    b[peer_loc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += sizeof(buffer_t::element) * INTERPROCESS_SLOTS_IN_BUFFER;

    q_emergency[myloc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    remoteaddr += locklessqueue_t<queue_t::element, 256>::getmemsize();
    q_emergency[peer_loc].initRDMA(_qp, _cq, _lkey, _rkey, remoteaddr);
    peer_loc += locklessqueue_t<queue_t::element, 256>::getmemsize();

    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(rd_mutex, &mutexattr) != 0)
        FATAL("Error to init mutex");


}
