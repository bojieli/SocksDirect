//
// Created by ctyi on 11/10/17.
//

#include "../common/helper.h"
#include "setup_sock_monitor.h"
#include "process.h"
#include "sock_monitor.h"
#include "../common/metaqueue.h"
#include<time.h>

#undef DEBUGON
#define DEBUGON 1

static void try_accept_new_proc()
{
    ctl_struc data;
    int fd;
    if ((fd = setup_sock_accept(&data)) != -1)
    {
        data.key=process_add(data.pid);
        DEBUG("process id: %d", data.pid);
        setup_sock_send(fd, &data);
        DEBUG("Ack sent!");
    }
}

static struct timespec time_start={0, 0},time_end={0, 0};
static long counter=0;

inline
static void thr_test_init_handler()
{
    counter=0;
    clock_gettime(CLOCK_REALTIME, &time_start);
}
inline
static void thr_test_handler(long cnt_recv)
{
    if (cnt_recv != counter) {
        printf("!!!\n");
        printf("%ld\n", cnt_recv);
        FATAL("Implementation error on counter %ld", counter);
    }
    ++counter;
    if ((counter & 0xFFFFFF) == 0xFFFFFF)
    {
        clock_gettime(CLOCK_REALTIME, &time_end);
        printf("16.7Mop time: %lf\n", time_end.tv_sec-time_start.tv_sec+
                ((double)time_end.tv_nsec-time_start.tv_nsec)/1E9);
        clock_gettime(CLOCK_REALTIME, &time_start);
    }
}
inline
static void ping_handler(metaqueue_element *req_body, metaqueue_element *res_body)
{
    *res_body = *req_body;
    (*res_body).data.command.data = ~req_body->data.command.data;
}
inline
static void event_processer(metaqueue_pack q_pack_req, metaqueue_pack q_pack_res, int qid)
{
    metaqueue_element req_body;
    metaqueue_element res_body;
    metaqueue_pop(q_pack_req, &req_body);
    switch (req_body.data.command.command){
        case REQ_THRTEST:
            thr_test_handler(req_body.data.command.data);
            break;
        case REQ_THRTEST_INIT:
            thr_test_init_handler();
            break;
        case REQ_PING:
            ping_handler(&req_body, &res_body);
            metaqueue_push(q_pack_res, &res_body);
            break;
        case REQ_LISTEN:
            listen_handler(&req_body, &res_body, qid);
            metaqueue_push(q_pack_res, &res_body);
            break;
        case REQ_CONNECT:
            connect_handler(&req_body, &res_body, qid);
            metaqueue_push(q_pack_res, &res_body);
            break;
        case REQ_NOP:
        default:
            break;
    }
}
static void event_loop()
{
    int current_pointer;
    uint8_t round=0;
    current_pointer = 0;
    while (process_current_counter == 0)
        try_accept_new_proc();
    while (1)
    {

        //main part of the event loop
        metaqueue_pack q_pack_req, q_pack_res;
        q_pack_req = process_getrequesthandler_byqid(current_pointer);
        q_pack_res = process_getresponsehandler_byqid(current_pointer);
        //if empty continue
        if (!metaqueue_isempty(q_pack_req))
            event_processer(q_pack_req, q_pack_res, current_pointer);
        ++current_pointer;
        if (current_pointer == process_current_counter) {
            ++round;
            current_pointer = 0;
            if ((round & 0xFFFF) == 0) {
                try_accept_new_proc();
            }
        }
    }

}
int main()
{
    pin_thread(0);
    setup_sock_monitor_init();
    process_init();
    event_loop();
    return 0;
}