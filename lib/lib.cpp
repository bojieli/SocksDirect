#include "../common/helper.h"
#include "../common/metaqueue.h"
#include "setup_sock_lib.h"
#include "lib.h"
#include "socket_lib.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include "lib_internal.h"
#include "../common/setup_sock.h"

pthread_key_t pthread_key;

#undef DEBUGON

#define DEBUGON 1

static void after_fork_father()
{
    thread_data_t* thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    while (true)
    {
        metaqueue_ctl_element fork_res;
        while(!thread_data->metaqueue.q[1].pop_nb(fork_res));
        if (fork_res.command == FIN_FORK)
        {
            DEBUG("Father: FORK FIN");
            break;
        }
        if (fork_res.command == RES_FORK)
        {
            DEBUG("Father: Old key: %u, New key: %u", fork_res.resp_fork.oldshmemkey, fork_res.resp_fork.newshmemkey);
            int old_buffer_id = (*(thread_sock_data->bufferhash))[fork_res.resp_fork.oldshmemkey];
            int loc = thread_sock_data->buffer[old_buffer_id].loc;
            int new_buffer_id = thread_sock_data->newbuffer(fork_res.resp_fork.newshmemkey, loc);
            //first process read side
            
            //iterate all the fd
            for (int fd = thread_data->fds_datawithrd.hiter_begin(); 
                 fd!=-1; 
                 fd = thread_data->fds_datawithrd.hiter_next(fd))
            {
                for (auto iter = thread_data->fds_datawithrd.begin(fd); !iter.end(); iter.next())
                {
                    if (iter->buffer_idx == old_buffer_id)
                    {
                        DEBUG("Matched for fd %d", fd);
                        iter->status |= FD_STATUS_FORKED;
                        iter->child[0] = new_buffer_id;
                    }
                }
            }
        }
    }
}
#undef DEBUGON
#define DEBUGON 1
static void after_fork_child(pid_t oldtid)
{
    thread_data_t* thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    metaqueue_ctl_element fork_req;
    fork_req.command = REQ_FORK;
    fork_req.req_fork.token = thread_data->old_token;
    //printf("old token %lu\n", thread_data->old_token);
    fork_req.req_fork.old_tid = oldtid;
    thread_data->metaqueue.q[0].push(fork_req);
    //receive all the fork request
    while (true)
    {
        metaqueue_ctl_element fork_resp;
        while(!thread_data->metaqueue.q[1].pop_nb(fork_resp));
        if (fork_resp.command == FIN_FORK)
        {
            DEBUG("Child: FORK FIN");
            break;
        }
        if (fork_resp.command == RES_ERROR)
            FATAL("Receive error from monitor during fork, err: %d", fork_resp.resp_command.err_code);
        if (fork_resp.command == RES_FORK)
            DEBUG("Child: Old key: %u, New key: %u", fork_resp.resp_fork.oldshmemkey, fork_resp.resp_fork.newshmemkey);
    }
}

#undef DEBUGON

static void connect_monitor()
{
    ctl_struc result;
    setup_sock_connect(&result);
    thread_data_t * data = GET_THREAD_DATA();
    data->uniq_shared_id = shmget(result.key, data->metaqueue.get_sharememsize(), 0777);
    data->token = result.token;
    //printf("%d\n", uniq_shared_id);
    if (data->uniq_shared_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    data->uniq_shared_base_addr = shmat(data->uniq_shared_id, NULL, 0);
    if (data->uniq_shared_base_addr == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    data->metaqueue.init_memlayout((uint8_t *)data->uniq_shared_base_addr, 1);

}

__attribute__((constructor))
void after_exec()
{
    pthread_key_create(&pthread_key, NULL);
    pthread_key_create(&pthread_sock_key, NULL);
    thread_data_t *data = new thread_data_t;
    pthread_setspecific(pthread_key, (void *) data);
    connect_monitor();
    usocket_init();
}

pid_t fork()
{
    thread_data_t * thread_data = GET_THREAD_DATA();
    pid_t oldtid = gettid();
    thread_data->old_token = thread_data->token;
    //printf("%lu\n", thread_data->token);
    pid_t result = ORIG(fork, ());
    if (result == -1)
        return result;
    if (result == 0)
    {
        //printf("0\n");
        connect_monitor();
        //printf("1\n");
        after_fork_child(oldtid);
    } else
    {
        after_fork_father();
    }
    return result;
}

#undef DEBUGON
#define DEBUGON 0
/*
void ipclib_sendmsg(int command, int data)
{
    metaqueue_element data2send;
    data2send.data.command.command = command;
    data2send.data.command.data = data;
    //data2send.data.command.tid=gettid();
    metaqueue_pack q_pack;
    thread_data_t *t_data = NULL;
    t_data = (thread_data_t *) pthread_getspecific(pthread_key);
    q_pack.data = &(t_data->metaqueue[0]);
    q_pack.meta = &(t_data->metaqueue_metadata[0]);
    metaqueue_push(q_pack, &data2send);
    DEBUG("Sent command %d to monitor", command);
}

void ipclib_recvmsg(metaqueue_element *data)
{
    metaqueue_pack q_pack;
    thread_data_t *t_data = NULL;
    t_data = (thread_data_t *) pthread_getspecific(pthread_key);
    q_pack.data = &(t_data->metaqueue[1]);
    q_pack.meta = &(t_data->metaqueue_metadata[1]);
    metaqueue_pop(q_pack, data);
    DEBUG("Recv from monitor");
} */

#undef DEBUGON
#define DEBUGON 0


struct wrapper_arg
{
    void *(*func)(void *);

    void *arg;
};

static void *wrapper(void *arg)
{
    thread_data_t backup_thread_data = *GET_THREAD_DATA();
    thread_sock_data_t backup_thread_sock_data = *GET_THREAD_SOCK_DATA();
    struct wrapper_arg *warg = (wrapper_arg *)arg;
    connect_monitor();
    usocket_init();
    thread_data_t * thread_data = GET_THREAD_DATA();
    thread_sock_data_t * thread_sock_data = GET_THREAD_SOCK_DATA();
    *thread_data = backup_thread_data;
    *thread_sock_data = backup_thread_sock_data;
    void *(*func)(void *) = warg->func;
    arg = warg->arg;
    free(warg);
    pthread_exit(func(arg));
    /* NORETURN */
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) __THROW
{
    struct wrapper_arg *warg = (wrapper_arg *)malloc(sizeof(*warg));
    if (!warg)
        FATAL("malloc() failed");
    warg->func = start_routine;
    warg->arg = arg;
    int result = ORIG(pthread_create, (thread, attr, wrapper, warg));
    /* XXX */
    return result;
}