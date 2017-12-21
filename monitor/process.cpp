//
// Created by ctyi on 11/14/17.
//

#include "../common/helper.h"
#include "process.h"
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <semaphore.h>
#include "../common/metaqueue.h"
#include "../common/darray.hpp"

static darray_t<process_sturc, MAX_PROCESS_NUM> process;

int process_current_counter = 0;

void process_init()
{
    process.init();
}

key_t process_add(pid_t pid)
{
    process_sturc curr_proc;
    int id=process.add(curr_proc);
    curr_proc.pid = pid;
    if ((curr_proc.uniq_shmem_id = ftok(SHM_NAME, id + 2)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    int shm_id = shmget(curr_proc.uniq_shmem_id, 2 * sizeof(metaqueue_data), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    //point metaqueue pointer of current process to assigned memory address
    curr_proc.metaqueue[0] = (metaqueue_data *) shmat(shm_id, NULL, 0);
    curr_proc.metaqueue[1] = (metaqueue_data *)
            ((uint8_t *) curr_proc.metaqueue[0] +
             sizeof(metaqueue_data));
    if (curr_proc.metaqueue[0] == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    memset(curr_proc.metaqueue[0], 0, 2 * sizeof(metaqueue_data));
    metaqueue_pack tmp_pack;
    tmp_pack.data = curr_proc.metaqueue[0];
    tmp_pack.meta = &curr_proc.metaqueue_meta[0];
    metaqueue_init_data(tmp_pack);
    metaqueue_init_meta(tmp_pack);
    tmp_pack.data = curr_proc.metaqueue[1];
    tmp_pack.meta = &curr_proc.metaqueue_meta[1];
    metaqueue_init_data(tmp_pack);
    metaqueue_init_meta(tmp_pack);
    key_t ret_val = curr_proc.uniq_shmem_id;
    process[id]=curr_proc;
    return ret_val;
}

metaqueue_pack process_getrequesthandler_byqid(int qid)
{
    metaqueue_pack result;
    result.data = process[qid].metaqueue[0];
    result.meta = &process[qid].metaqueue_meta[0];
    return result;
}

metaqueue_pack process_getresponsehandler_byqid(int qid)
{
    metaqueue_pack result;
    result.data = process[qid].metaqueue[1];
    result.meta = &process[qid].metaqueue_meta[1];
    return result;
}

int process_iterator_init()
{
    return process.iterator_init();
}

int process_iterator_next(int prev)
{
    return process.iterator_next(prev);
}
