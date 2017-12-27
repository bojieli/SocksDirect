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
        data.key = process_add(data.pid, data.tid);
        DEBUG("process id: %d, thread id: %d", data.pid, data.tid);
        setup_sock_send(fd, &data);
        DEBUG("Ack sent!");
    }
}

static struct timespec time_start = {0, 0}, time_end = {0, 0};
static long counter = 0;

inline
static void thr_test_init_handler()
{
    counter = 0;
    clock_gettime(CLOCK_REALTIME, &time_start);
}

inline
static void thr_test_handler(long cnt_recv)
{
    if (cnt_recv != counter)
    {
        printf("!!!\n");
        printf("%ld\n", cnt_recv);
        FATAL("Implementation error on counter %ld", counter);
    }
    ++counter;
    if ((counter & 0xFFFFFF) == 0xFFFFFF)
    {
        clock_gettime(CLOCK_REALTIME, &time_end);
        printf("16.7Mop time: %lf\n", time_end.tv_sec - time_start.tv_sec +
                                      ((double) time_end.tv_nsec - time_start.tv_nsec) / 1E9);
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
    switch (req_body.data.command.command)
    {
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
        case REQ_CLOSE:
            close_handler(&req_body, qid);
            break;
        case REQ_NOP:
        default:
            break;
    }
}

static void event_loop()
{
    unsigned int round = 0;
    while (1)
    {
       for (int i=process_iterator_init();i!=-1;i=process_iterator_next(i))
       {
           metaqueue_pack q_pack_req, q_pack_res;
           q_pack_req = process_getrequesthandler_byqid(i);
           q_pack_res = process_getresponsehandler_byqid(i);
           //if empty continue
           if (!metaqueue_isempty(q_pack_req))
               event_processer(q_pack_req, q_pack_res, i);
       }
        ++round;
        if ((round & 0xFFFF) == 0)
        {
            try_accept_new_proc();
            process_chk_remove();
            sock_resource_gc();
        }
    }

}

int main()
{
    pin_thread(4);
    setup_sock_monitor_init();
    process_init();
    event_loop();
    return 0;
}
