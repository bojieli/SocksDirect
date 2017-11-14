#include "../common/helper.h"
#include "../common/attachqueue.h"
#include "attachqueue_lib.h"
#include "../common/metaqueue.h"
static key_t uniq_shared_base_key;
static int uniq_shared_id;
static void* uniq_shared_base_addr;
static metaqueue_data* metaqueue;
static void* metaqueue_pointer[2];
void connect_monitor()
{
    attachqueue_init();
    ctl_struc process_data, result;
    process_data.pid = gettid();
    process_data.key = 0;
    attachqueue_pushreq(process_data);
    result = attachqueue_pullack(process_data.pid);
    uniq_shared_base_key = result.key;
    uniq_shared_id = shmget(result.key, 2*sizeof(aqueue_struc), 0777);
    if (uniq_shared_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    uniq_shared_base_addr = shmat(uniq_shared_id, NULL, 0);
    if (uniq_shared_base_addr == (void*)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    metaqueue = (metaqueue_data*) uniq_shared_base_addr;
    metaqueue_pointer[0]=metaqueue_pointer[1]=0;
    DEBUG("monitor_hooked");
}
__attribute__((constructor))
void after_exec()
{
    connect_monitor();
}