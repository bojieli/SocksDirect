#include "../common/helper.h"
#include "../common/metaqueue.h"
#include "setup_sock_lib.h"
#include "lib.h"
#include <sys/ipc.h>
#include <sys/shm.h>
static int uniq_shared_id;
static void* uniq_shared_base_addr;
//0 is to monitor 1 is from monitor
static metaqueue_data* metaqueue;
static metaqueue_meta metaqueue_metadata[2];
void connect_monitor()
{
    ctl_struc result;
    setup_sock_connect(&result);
    uniq_shared_id = shmget(result.key, 2*sizeof(metaqueue_data), 0777);
    //printf("%d\n", uniq_shared_id);
    if (uniq_shared_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    uniq_shared_base_addr = shmat(uniq_shared_id, NULL, 0);
    if (uniq_shared_base_addr == (void*)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    metaqueue = (metaqueue_data*) uniq_shared_base_addr;
    metaqueue_metadata[0].pointer=metaqueue_metadata[1].pointer=0;

}
__attribute__((constructor))
void after_exec()
{
    connect_monitor();
}
pid_t fork() {
    pid_t result=ORIG(fork, ());
    if (result == -1)
        return result;
    if (result==0) {
        connect_monitor();
    }
}
#undef DEBUGON
#define DEBUGON 0
void ipclib_sendmsg(int command, int data)
{
    metaqueue_element data2send;
    data2send.data.command.command=command;
    data2send.data.command.data=data;
    //data2send.data.command.pid=gettid();
    metaqueue_pack q_pack;
    q_pack.data = &metaqueue[0];
    q_pack.meta = &metaqueue_metadata[0];
    metaqueue_push(q_pack, &data2send);
    DEBUG("Sent command %d to monitor", command);
}
void ipclib_recvmsg(metaqueue_element *data)
{
    metaqueue_pack q_pack;
    q_pack.data = &metaqueue[1];
    q_pack.meta = &metaqueue_metadata[1];
    metaqueue_pop(q_pack, data);
    DEBUG("Recv from monitor");
}
#undef DEBUGON
#define DEBUGON 0