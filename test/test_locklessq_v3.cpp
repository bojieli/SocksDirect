//
// Created by ctyi on 1/2/18.
//

#include <pthread.h>
#include <cstdlib>
#include "../common/locklessq_v3.hpp"
#include "../common/helper.h"

locklessqueue_t_v3<uint64_t, 256> queue_sender, queue_receiver;
uint64_t glbcounter(0);
static void* reader(void* param)
{
    (void)param;
    pin_thread(0);
    uint64_t ret;
    locklessqueue_t_v3<uint64_t , 256>::iterator iter = queue_receiver.begin();
    while (!iter.valid()) iter = queue_receiver.begin();
    while (true)
    {
        while (!iter->isvalid) SW_BARRIER;
        //if (glbcounter == 511)
        //    printf("read: %lu\n", glbcounter);
        if (iter->data != glbcounter)
            FATAL("data error %lu", ret);
        ++glbcounter;
        iter.del();
        //++iter;
    }
    return nullptr;
}

static void* writer(void* param)
{
    pin_thread(2);
    (void)param;
    uint64_t counter(0);
    while (true)
    {
        while (!queue_sender.push_nb(counter));
        //printf("write: %lu\n", counter);
        ++counter;
    }
    return nullptr;
}

int main()
{
    void* baseaddr;
    bool flag[65];
    void *blk1 = malloc(16*256);
    void *blk2 = malloc(16*256);
    locklessqueue_t_v3<uint64_t, 256>::mem_ptr_t ptr1, ptr2;

    ptr1.return_flag = &flag[0];
    ptr1.ringbuffer = (locklessqueue_t_v3<uint64_t, 256>::element_t *)blk1;

    ptr2.return_flag = &flag[64];
    ptr2.ringbuffer = (locklessqueue_t_v3<uint64_t, 256>::element_t *)blk2;
    queue_sender.init_ptr(ptr1,ptr2, false);
    queue_sender.init_mem();
    queue_receiver.init_ptr(ptr1, ptr2, true);
    /*
    for (int i = 0; i < 5; ++i)
    {
        queue_sender.push(i);
    }
    queue_receiver.del(2);
    queue_receiver.del(1);
    queue_receiver.del(0);
    if (queue_receiver.pointer != 3)
        FATAL("queue pointer error");
    queue_receiver.del(3);
    queue_receiver.del(4);
     */
    pthread_t sendthread, recvthread;
    pthread_create(&sendthread, NULL, writer, NULL);
    pthread_create(&recvthread, NULL, reader, NULL);
    while (1)
    {
        sleep(1);
        printf("%ldM\n", glbcounter/1000000);
    }
    return 0;
}
