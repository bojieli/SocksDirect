//
// Created by ctyi on 12/6/17.
//

#include <cstdio>
#include <cstdlib>
#include <random>
#include "../common/interprocess_t.h"
#include "../common/helper.h"

interprocess_t interprocess_w;
interprocess_t interprocess_r;
pthread_t sendthread, recvthread;
void *baseaddr;
int glb_counter=0;

const int total_buffer_size = 8 * INTERPROCESS_SLOTS_BLK_SIZE;
uint8_t test_data[total_buffer_size];
uint8_t output_data[total_buffer_size];

static void* reader(void* param)
{
    pin_thread(0);
    int counter=0;
    interprocess_t::queue_t::element element;
    while (1)
    {
        interprocess_r.q[1].pop(element);
        if (*(int*)element.raw != counter)
            FATAL("data error");
        ++counter;
    }
    return nullptr;
}

static void* writer(void* param)
{
    pin_thread(2);
    interprocess_t::queue_t::element element;
    while (1)
    {
        *(int*)element.raw=glb_counter;
        interprocess_w.q[0].push(element);
        ++glb_counter;
    }
    return nullptr;
}

static void init()
{
    interprocess_t::monitor_init(baseaddr);
    interprocess_w.init(baseaddr, 0);
    interprocess_r.init(baseaddr, 1);
    pthread_create(&sendthread, NULL, writer, NULL);
    pthread_create(&recvthread, NULL, reader, NULL);
    while (1)
    {
        sleep(1);
        printf("%dM\n", glb_counter/1000000);
    }
}



int main()
{
    pin_thread(0);
    baseaddr=malloc(interprocess_t::get_sharedmem_size());
    interprocess_t::monitor_init(baseaddr);
    interprocess_w.init(baseaddr, 0);
    interprocess_r.init(baseaddr, 1);

    //test the buffer
    //generate the test data
    for (int i = 0; i < total_buffer_size; ++i)
        test_data[i] = rand() % 256;

    //full push pop
    for (int times = 0; times < 200; ++times)
    {
        int startblk;
        int rdsize = total_buffer_size;
        startblk = interprocess_w.b[0].pushdata(test_data, total_buffer_size);
        interprocess_r.b[1].popdata(startblk, rdsize, output_data);
        if (rdsize != total_buffer_size)
            FATAL("Read size is different");
        if (memcmp(output_data, test_data, total_buffer_size) != 0)
            FATAL("Read data different");
    }

    //less than a block
    int startblk = interprocess_w.b[0].pushdata(test_data, INTERPROCESS_SLOTS_BLK_SIZE * 3);
    uint8_t single_byte;
    int pop_size;
    for (int i = 0; i < INTERPROCESS_SLOTS_BLK_SIZE * 3; ++i)
    {
        pop_size = 1;
        startblk = interprocess_r.b[1].popdata(startblk, pop_size, &single_byte);
        if (pop_size != 1)
            FATAL("Read size is different");
        if (single_byte != test_data[i])
            FATAL("Read data different %d", i);
    }

    //2 times, each time two and a half
    startblk = interprocess_w.b[0].pushdata(test_data, INTERPROCESS_SLOTS_BLK_SIZE * 5);
    int twohalf_size = 2 * INTERPROCESS_SLOTS_BLK_SIZE + 0.5 * INTERPROCESS_SLOTS_BLK_SIZE;
    pop_size = twohalf_size;
    startblk = interprocess_r.b[1].popdata(startblk, pop_size, output_data);
    if (pop_size != twohalf_size)
        FATAL("Read size is different");
    startblk = interprocess_r.b[1].popdata(startblk, pop_size, (uint8_t *) output_data + twohalf_size);
    if (pop_size != twohalf_size)
        FATAL("Read size is different");
    if (memcmp(output_data, test_data, 5 * INTERPROCESS_SLOTS_BLK_SIZE) != 0)
        FATAL("Read data error");

    startblk = interprocess_w.b[0].pushdata(test_data, twohalf_size);
    pop_size = 5 * INTERPROCESS_SLOTS_BLK_SIZE;
    startblk = interprocess_r.b[1].popdata(startblk, pop_size, output_data);
    if (pop_size != twohalf_size)
        FATAL("Read size different");
    if (memcmp(output_data, test_data, twohalf_size) != 0)
        FATAL("Read data error");

    for (int times = 0; times < 10; ++times)
    {
        interprocess_t::queue_t::element element;
        for (int i = 0; i < INTERPROCESS_SLOTS_IN_QUEUE; ++i)
        {
            element.data_fd_rw.fd = test_data[i];
            interprocess_w.q[0].push(element);
        }
        for (int i = 0; i < INTERPROCESS_SLOTS_IN_QUEUE; ++i)
        {
            interprocess_r.q[1].pop(element);
            if (element.data_fd_rw.fd != test_data[i])
                FATAL("Queue data error");
        }
    }
    interprocess_t::queue_t::element element;
    for (int i = 0; i < 5; ++i)
    {
        element.data_fd_rw.fd = test_data[i];
        interprocess_w.q[0].push(element);
    }
    interprocess_r.q[1].del(2);
    interprocess_r.q[1].del(1);
    interprocess_r.q[1].del(0);
    if (interprocess_r.q[1].tail != 3)
        FATAL("queue pointer error");

    printf("interprocess queue test success\n");
    init();
    return 0;
}
