//
// Created by ctyi on 6/12/18.
//

#include <cstdio>
#include <cstring>
#include "../common/locklessq_v2.hpp"
#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../common/helper.h"
#define MMAP_FILENAME "/dev/shm/test_q"
locklessq_v2 test_q_sender, test_q_receiver;
void* startaddr(nullptr),  *duplicate_addr(nullptr), *initaddr(nullptr);

void* sender(void* args)
{
    (void)args;
    int counter=0;
    uint8_t payloads[2048];
    for (int i=0;i<2048;++i)
        payloads[i]=i;
    while (counter < 1000000)
    {
        locklessq_v2::element_t ele;
        ele.fd = counter;
        ele.command = 3;
        ele.flags = LOCKLESSQ_BITMAP_ISVALID;
        ele.size = counter % 128 * 16;
        ele.inner_element.data_fd_rw.pointer=0;
        while (!test_q_sender.push_nb(ele, &payloads[0])) SW_BARRIER;
        ++counter;
    }
return nullptr;
}

void* receiver(void* args)
{
    (void)args;
    int counter=0;
    uint8_t payloads[2048];
    int pointer=test_q_receiver.pointer;
    while (counter < 1000000)
    {
        locklessq_v2::element_t ele;
        do {
            ele = test_q_receiver.peek_meta(pointer);
            SW_BARRIER;
        } while (!(ele.flags & LOCKLESSQ_BITMAP_ISVALID));
        memcpy(&payloads[0], test_q_receiver.peek_data(pointer), ele.size);

        //Begin verify
        if (ele.size != counter % 128 * 16)
            FATAL("Meta size err");
        if (ele.flags & LOCKLESSQ_BITMAP_ISDEL)
            FATAL("element is deleted");

        if (ele.fd != counter)
            FATAL("FD number error");
        if (ele.command != 3)
            FATAL("Command error");
        pointer = test_q_receiver.del(pointer);
        //start to verify data
        for (int i=0;i<ele.size;++i)
        {
            if (payloads[i] != (uint8_t)i)
                FATAL("data err");
        }
        ++counter;
    }
    return nullptr;
}
int main()
{
    int shmfd = open(MMAP_FILENAME,  O_RDWR | O_CREAT,S_IRWXU | S_IRWXG | S_IRWXO);
    int total_size=locklessq_v2::getalignedmemsize() + ((locklessq_v2::getmemsize()-1)/(4*1024)+1)*(4*1024);

    if (shmfd == -1)
        FATAL("Failed to create shm file with error %s", strerror(errno));

    fallocate(shmfd,0,0,total_size);

    if (locklessq_v2::getalignedmemsize() != 8*1024)
        FATAL("Error queue size");
    initaddr = mmap(NULL, total_size,PROT_READ | PROT_WRITE, MAP_SHARED,shmfd,0);
    if (initaddr == NULL)
        FATAL("Init mmap failed");
    munmap(initaddr, total_size);
    startaddr = mmap(initaddr, locklessq_v2::getalignedmemsize(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,shmfd,0);
    *(int *)startaddr=1;
    if (static_cast<int>(reinterpret_cast<std::intptr_t>(startaddr)) == -1)
        FATAL("Failed to map the first part of shmem with error %s", strerror(errno));
    if (startaddr != initaddr)
        FATAL("Failed to map the first part at specific addr");
    duplicate_addr = mmap(startaddr + locklessq_v2::getalignedmemsize(),
            locklessq_v2::getmemsize(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,shmfd,0);
    if (static_cast<int>(reinterpret_cast<std::intptr_t>(duplicate_addr)) == -1)
        FATAL("Failed to map the second part of shmem with error %s", strerror(errno));
    if (duplicate_addr != startaddr +  locklessq_v2::getalignedmemsize())
        FATAL("Two shared mem not adjacent");

    pthread_t sender_thread, receiver_thread;
    test_q_sender.init(startaddr,false);
    test_q_receiver.init(startaddr, true);
    test_q_sender.init_mem();
    pthread_create(&sender_thread, NULL, sender, nullptr);
    pthread_create(&receiver_thread, NULL, receiver, nullptr);
    pthread_join(receiver_thread,NULL);
    return 0;
}