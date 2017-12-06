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
    interprocess.q[1].init(reinterpret_cast<interprocess_t::queue_t::data_t *>(baseaddr) + 1);
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
    
    return 0;
}