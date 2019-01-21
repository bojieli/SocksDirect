#include "../common/helper.h"
#include "../common/metaqueue.h"
#include "setup_sock_lib.h"
#include "lib.h"
#include "socket_lib.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include "lib_internal.h"
#include "fork.h"
#include "../common/setup_sock.h"
#include "rdma_lib.h"

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
            fd_rd_list_t tmp_fd_list_ele;
            tmp_fd_list_ele.child[0] = tmp_fd_list_ele.child[1] = -1;
            tmp_fd_list_ele.buffer_idx = new_buffer_id;
            tmp_fd_list_ele.status = 0;
            //first process read side
            
            //iterate all the fd
            for (int fd = thread_data->fds_datawithrd.hiter_begin(); 
                 fd!=-1; 
                 fd = thread_data->fds_datawithrd.hiter_next(fd))
            {
                bool isFind(false);
                for (auto iter = thread_data->fds_datawithrd.begin(fd); !iter.end(); iter.next())
                {
                    if (iter->buffer_idx == old_buffer_id)
                    {
                        DEBUG("Matched for fd %d", fd);
                        iter->buffer_idx = new_buffer_id;
                        iter->status = 0;
                        isFind = true;
                    } else
                    {
                        //search for the subtree of current adjlist element
                        int ret(-1);
                        if (iter->child[0] != -1)
                        {
                            ret = fork_traverse_rd_tree(iter->child[0], old_buffer_id);
                        }
                        //not found in the left side, try right side
                        if ((ret == -1) && (iter->child[1] != -1))
                            ret = fork_traverse_rd_tree(iter->child[1], old_buffer_id);
                        //if found
                        if (ret != -1)
                        {
                            isFind = true;
                            thread_data->rd_tree[ret].status |= FD_STATUS_RD_RECV_FORKED;
                            iter->status = 0;
                            thread_data->rd_tree[ret].buffer_idx = new_buffer_id;
                        }
                    }
                }

            }
            
            //deal with the write side
            //directly replace the old one with the new one
            //first iterate and replace
            for (int fd = thread_data->fds_wr.hiter_begin();
                 fd!=-1;
                 fd = thread_data->fds_wr.hiter_next(fd))
            {
                //first iterate adjlist
                for (auto iter = thread_data->fds_wr.begin(fd); !iter.end(); iter=iter.next())
                {
                    if (iter->buffer_idx == old_buffer_id)
                    {
                        DEBUG("Write Fork case: find old buffer idx in adjlist for fd %d", fd);
                        iter->status = 0;
                        iter->buffer_idx = new_buffer_id;
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
    metaqueue_ctl_element fork_req;
    fork_req.command = REQ_FORK;
    fork_req.req_fork.token = thread_data->old_token;
    //printf("old token %lu\n", thread_data->old_token);
    fork_req.req_fork.old_tid = oldtid;
    thread_data->metaqueue.q[0].push(fork_req);
    after_fork_father();
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
    rdma_init();
}

/*
static bool recv_takeover_check(int idx)
{
    //whether itself is the leaf
    thread_data_t * thread_data = GET_THREAD_DATA();
    bool ret(true);
    if (thread_data->rd_tree[idx].status | FD_STATUS_RECV_REQ)
        ret = (bool)(thread_data->rd_tree[idx].status | FD_STATUS_RECV_ACK);
    
    //clear the label if current is finished
    if (ret)
    {
        thread_data->rd_tree[idx].status &= ~FD_STATUS_RECV_REQ;
        thread_data->rd_tree[idx].status &= ~FD_STATUS_RECV_ACK;
    }
    
    if (thread_data->rd_tree[idx].child[0] != -1)
        ret = ret && recv_takeover_check(thread_data->rd_tree[idx].child[0]);

    if (thread_data->rd_tree[idx].child[1] != -1)
        ret = ret && recv_takeover_check(thread_data->rd_tree[idx].child[1]);
    
    return ret;
    
} */

/*

static bool before_fork_blocking_chk()
{
    //before fork, the program needs to check whether all the ack is received for the read side
    thread_data_t * thread_data = GET_THREAD_DATA();
    bool isblocking(false);
    for (int fd = thread_data->fds_datawithrd.hiter_begin();
            fd != -1;
            fd = thread_data->fds_datawithrd.hiter_next(fd))
    {
        if (thread_data->fds_datawithrd[fd].property.status & FD_STATUS_RECV_REQ)
        {
            //iterate all the adjlist of this fd
            for (auto iter = thread_data->fds_datawithrd.begin(fd); !iter.end(); iter = iter.next())
            {
                if (iter->status & FD_STATUS_RECV_REQ)
                {
                    isblocking = isblocking && (bool)(iter->status & FD_STATUS_RECV_ACK);
                }
                if (iter->child[0] != -1)
                    isblocking = isblocking && recv_takeover_check(iter->child[0]);
                if (iter->child[1] != -1)
                    isblocking = isblocking && recv_takeover_check(iter->child[1]);
            }
        }
    }
    return isblocking;
} */

/*void before_fork_blocking()
{
    while (before_fork_blocking_chk())
        monitor2proc_hook();
}*/

pid_t fork()
{
    //before_fork_blocking();
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
    thread_data_t * thread_data;
    thread_sock_data_t * thread_sock_data;

    void *(*func)(void *);
    void *arg;
};

static void *wrapper(void *arg)
{
    // initialize thread data structure
    thread_data_t * thread_data = new thread_data_t;
    pthread_setspecific(pthread_key, (void *) thread_data);
    // connect to monitor and rdma
    connect_monitor();
    usocket_init();
    rdma_init();

    struct wrapper_arg * warg = (struct wrapper_arg *) arg;
    /*
    // replace with original thread data
    // currently not implemented

    *thread_data = *(warg->thread_data);
    *thread_sock_data = *(warg->thread_sock_data);
    */

    // call start func
    void *(*start_func)(void *) = warg->func;
    void *real_arg = warg->arg;
    free(warg);
    pthread_exit(start_func(real_arg));
    /* NORETURN */
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) __THROW
{
    struct wrapper_arg *warg = (wrapper_arg *)malloc(sizeof(*warg));
    if (!warg)
        FATAL("malloc() failed");
    warg->thread_data = GET_THREAD_DATA();
    warg->thread_sock_data = GET_THREAD_SOCK_DATA();
    warg->func = start_routine;
    warg->arg = arg;
    int result = ORIG(pthread_create, (thread, attr, wrapper, warg));
    /* XXX */
    return result;
}
