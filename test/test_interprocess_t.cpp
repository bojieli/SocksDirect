//
// Created by ctyi on 12/6/17.
//

#include <cstdio>
#include <cstdlib>
#include <random>
#include "../common/interprocess_t.h"
#include "../common/helper.h"
interprocess_t interprocess;
int main()
{
    void *baseaddr=malloc(interprocess_t::get_sharedmem_size());
    interprocess.q[0].init(reinterpret_cast<interprocess_t::queue_t::data_t *>(baseaddr));
    interprocess.q[1].init(reinterpret_cast<interprocess_t::queue_t::data_t *>(baseaddr));
    interprocess.b[0] = reinterpret_cast<interprocess_t::buffer_t *>((uint8_t *) baseaddr + 2 * sizeof(interprocess_t::queue_t::data_t));
    interprocess.b[1] = reinterpret_cast<interprocess_t::buffer_t *>((uint8_t *) baseaddr + 2 * sizeof(interprocess_t::queue_t::data_t)) + 1;
    interprocess.q[0].clear();
    interprocess.q[1].clear();
    interprocess.b[0]->init();
    interprocess.b[1]->init();

    //test the buffer

    //generate the test data
    int total_buffer_size=8*INTERPROCESS_SLOTS_BLK_SIZE;
    uint8_t test_data[total_buffer_size];
    uint8_t output_data[total_buffer_size];
    for (int i=0;i<total_buffer_size;++i)
        test_data[i]=rand()%256;

    //full push pop
    for (int times=0;times<200;++times)
    {
        int startblk;
        int rdsize=total_buffer_size;
        startblk = interprocess.b[0]->pushdata(test_data, total_buffer_size);
        interprocess.b[0]->popdata(startblk, rdsize, output_data);
        if (rdsize != total_buffer_size)
            FATAL("Read size is different");
        if (memcmp(output_data, test_data, total_buffer_size) != 0)
            FATAL("Read data different");

    }

    //less than a block
    int startblk=interprocess.b[0]->pushdata(test_data, INTERPROCESS_SLOTS_BLK_SIZE*3);
    uint8_t single_byte;
    int pop_size;
    for (int i=0;i<INTERPROCESS_SLOTS_BLK_SIZE*3;++i)
    {
        pop_size = 1;
        startblk = interprocess.b[0]->popdata(startblk, pop_size, &single_byte);
        if (pop_size != 1)
            FATAL("Read size is different");
        if (single_byte != test_data[i])
            FATAL("Read data different %d",i);

    }

    //2 times, each time two and a half
    startblk=interprocess.b[0]->pushdata(test_data, INTERPROCESS_SLOTS_BLK_SIZE*5);
    int twohalf_size=2*INTERPROCESS_SLOTS_BLK_SIZE+0.5*INTERPROCESS_SLOTS_BLK_SIZE;
    pop_size=twohalf_size;
    startblk=interprocess.b[0]->popdata(startblk, pop_size, output_data);
    if (pop_size!=twohalf_size)
        FATAL("Read size is different");
    startblk=interprocess.b[0]->popdata(startblk, pop_size, (uint8_t *)output_data+twohalf_size);
    if (pop_size!=twohalf_size)
        FATAL("Read size is different");
    if (memcmp(output_data, test_data, 5*INTERPROCESS_SLOTS_BLK_SIZE) != 0)
        FATAL("Read data error");

    startblk=interprocess.b[0]->pushdata(test_data, twohalf_size);
    pop_size=5*INTERPROCESS_SLOTS_BLK_SIZE;
    startblk=interprocess.b[0]->popdata(startblk, pop_size, output_data);
    if (pop_size != twohalf_size)
        FATAL("Read size different");
    if (memcmp(output_data, test_data, twohalf_size) != 0)
        FATAL("Read data error");

    for (int times=0;times<10;++times)
    {
        interprocess_t::queue_t::element element;
        for (int i=0;i<INTERPROCESS_SLOTS_IN_QUEUE;++i)
        {
            element.data_fd_rw.fd=test_data[i];
            interprocess.q[0].push(element);
        }
        for (int i=0;i<INTERPROCESS_SLOTS_IN_QUEUE;++i)
        {
            interprocess.q[1].pop(element);
            if (element.data_fd_rw.fd != test_data[i])
                FATAL("Queue data error");
        }
    }
    interprocess_t::queue_t::element element;
    for (int i=0;i<5;++i)
    {
        element.data_fd_rw.fd=test_data[i];
        interprocess.q[0].push(element);
    }
    interprocess.q[1].del(2);
    interprocess.q[1].del(1);
    interprocess.q[1].del(0);
    if (interprocess.q[1].tail != 3)
        FATAL("queue pointer error");

    interprocess.q[0].init(reinterpret_cast<interprocess_t::queue_t::data_t *>(baseaddr));
    interprocess.q[1].init(reinterpret_cast<interprocess_t::queue_t::data_t *>(baseaddr));
    interprocess.q[0].clear();
    interprocess.q[1].clear();
    for (int i=0;i<5;++i)
    {
        element.data_fd_rw.fd=test_data[i];
        interprocess.q[0].push(element);
    }
    interprocess.q[1].del(2);
    interprocess.q[1].del(1);
    if (interprocess.q[1].tail != 0)
        FATAL("queue pointer error");
    printf("interprocess queue test success");
    return 0;
}