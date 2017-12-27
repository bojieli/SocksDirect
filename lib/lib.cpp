#include "../common/helper.h"
#include "../common/metaqueue.h"
#include "setup_sock_lib.h"
#include "lib.h"
#include "socket_lib.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include "lib_internal.h"

pthread_key_t pthread_key;

void connect_monitor()
{
    ctl_struc result;
    setup_sock_connect(&result);
    thread_data_t *data = (thread_data_t *) malloc(sizeof(thread_data_t));
    pthread_setspecific(pthread_key, (void *) data);
    data->uniq_shared_id = shmget(result.key, 2 * sizeof(metaqueue_data), 0777);
    //printf("%d\n", uniq_shared_id);
    if (data->uniq_shared_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    data->uniq_shared_base_addr = shmat(data->uniq_shared_id, NULL, 0);
    if (data->uniq_shared_base_addr == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    data->metaqueue = (metaqueue_data *) data->uniq_shared_base_addr;
    data->metaqueue_metadata[0].pointer = data->metaqueue_metadata[1].pointer = 0;

}

__attribute__((constructor))
void after_exec()
{
    pthread_key_create(&pthread_key, NULL);
    pthread_key_create(&pthread_sock_key, NULL);
    connect_monitor();
    usocket_init();
}

pid_t fork()
{
    pid_t result = ORIG(fork, ());
    if (result == -1)
        return result;
    if (result == 0)
    {
        connect_monitor();
    }
}

#undef DEBUGON
#define DEBUGON 0

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
}

#undef DEBUGON
#define DEBUGON 0


struct wrapper_arg
{
    void *(*func)(void *);

    void *arg;
};

static void *wrapper(void *arg)
{
    struct wrapper_arg *warg = (wrapper_arg *)arg;
    connect_monitor();
    usocket_init();
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