//
// Created by ctyi on 11/15/17.
//
#include "../common/helper.h"
#include "../common/metaqueue.h"
metaqueue_data q;
metaqueue_meta_t meta;
int glb_counter=0;
void* q_send(void* arg)
{
    pin_thread(0);
    metaqueue_meta_t meta;
    metaqueue_pack q_pack;
    q_pack.meta = &meta;
    q_pack.data = &q;
    metaqueue_init_meta(q_pack);
    long counter = 0;
    metaqueue_element data;
    while (1)
    {
        data.data.command.data = counter;
        ++counter;
        metaqueue_push(q_pack, &data);
    }
}
void *q_recv(void* arg)
{
    pin_thread(2);
    metaqueue_meta_t meta;
    metaqueue_pack q_pack;
    q_pack.data = &q;
    q_pack.meta = &meta;
    metaqueue_init_meta(q_pack);
    metaqueue_element data;
    while (1)
    {
        metaqueue_pop(q_pack, &data);
        if (glb_counter != data.data.command.data)
            FATAL("Error result on counter %d", glb_counter);
        ++glb_counter;
    }
}
int main() {
    metaqueue_pack q_pack;
    q_pack.data = &q;
    q_pack.meta = &meta;
    metaqueue_init_data(q_pack);
    pthread_t sendthread, recvthread;
    pthread_create(&sendthread, NULL, q_send, NULL);
    pthread_create(&recvthread, NULL, q_recv, NULL);
    pin_thread(4);
    while (1) {
        sleep(1);
        printf("counter: %dk\n", glb_counter / 1000);
    }

}