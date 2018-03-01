//
// Created by ctyi on 1/2/18.
//

#include <pthread.h>
#include <cstdlib>
#include "../common/locklessqueue_n.hpp"
#include "../common/helper.h"

locklessqueue_t<uint64_t, 1024> queue_sender, queue_receiver;
uint64_t glbcounter(0);
static void* reader(void* param)
{
    (void)param;
    pin_thread(0);
    uint64_t ret;
    while (true)
    {
        while (!queue_receiver.pop_nb(ret));
        //printf("read: %lu\n", glbcounter);
        if (ret != glbcounter)
            FATAL("data error %lu", ret);
        ++glbcounter;
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
        queue_sender.push(counter);
        //printf("write: %lu\n", counter);
        ++counter;
    }
    return nullptr;
}

int main()
{
    void* baseaddr;
    queue_sender.init(baseaddr=malloc(queue_sender.getmemsize()));
    queue_sender.init_mem();
    queue_receiver.init(baseaddr);
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