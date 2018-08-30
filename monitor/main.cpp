//
// Created by ctyi on 11/10/17.
//

#include "../common/helper.h"
#include "setup_sock_monitor.h"
#include "process.h"
#include "sock_monitor.h"
#include "../common/metaqueue.h"
#include "../common/setup_sock.h"
#include "rdma_monitor.h"
#include<time.h>
#include <tuple>

#undef DEBUGON
#define DEBUGON 0

static void try_accept_new_proc()
{
    ctl_struc data;
    int fd;
    if ((fd = setup_sock_accept(&data)) != -1)
    {
        std::tie(data.key,data.token) = process_add(data.pid, data.tid);
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
static void ping_handler(metaqueue_ctl_element *req_body, metaqueue_ctl_element *res_body)
{
    *res_body = *req_body;
    (*res_body).test_payload = ~req_body->test_payload;
}


inline
static void event_processer(metaqueue_t *q, int qid)
{
    metaqueue_ctl_element req_body;
    metaqueue_ctl_element res_body;
    while (!q->q[1].pop_nb(req_body));
    switch (req_body.command)
    {
        case REQ_THRTEST:
            thr_test_handler(*(long *)req_body.raw);
            break;
        case REQ_THRTEST_INIT:
            thr_test_init_handler();
            break;
        case REQ_PING:
            ping_handler(&req_body, &res_body);
            q->q[0].push(res_body);
            break;
        case REQ_LISTEN:
            listen_handler(&req_body, &res_body, qid);
            q->q[0].push(res_body);
            break;
        case REQ_CONNECT:
            connect_handler(&req_body, &res_body, qid);
            q->q[0].push(res_body);
            break;
        case REQ_CLOSE:
            close_handler(&req_body, qid);
            break;
        case REQ_FORK:
            fork_handler(&req_body, qid);
            break;
        case REQ_RELAY_RECV:
            recv_takeover_handler(&req_body, qid);
            break;
        case REQ_NOP:
            //printf("%x\n", *((int *)&req_body.raw));
            break;
        case LONG_MSG_HEAD:
            long_msg_handler(&req_body, qid);
            break;
        case RDMA_QP_ACK:
            rdma_ack_handler(&req_body, qid);
        default:
            break;
    }
}

static void event_loop()
{
    unsigned int round = 0;
    while (true)
    {
       for (int i=process_iterator_init();i!=-1;i=process_iterator_next(i))
       {
           metaqueue_t * q;
           q = process_gethandler_byqid(i);
           //if empty continue
           if (!q->q[1].isempty())
               event_processer(q, i);
       }
        ++round;
        if ((round & 0xFFFF) == 0)
        {
            try_accept_new_proc();
            try_new_rdma();
            process_chk_remove();
            sock_resource_gc();
        }
    }

}

int main()
{
    //pin_thread(31);
    setup_sock_monitor_init();
    rdma_init();
    sock_monitor_init();
    process_init();
    event_loop();
    return 0;
}
