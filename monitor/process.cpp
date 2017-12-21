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

static process_sturc process[MAX_PROCESS_NUM];

int process_current_counter = 0;

void process_init()
{
    process_current_counter = 0;
}

key_t process_add(pid_t pid)
{
    process[process_current_counter].pid = pid;
    if ((process[process_current_counter].uniq_shmem_id = ftok(SHM_NAME, process_current_counter + 2)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    int shm_id = shmget(process[process_current_counter].uniq_shmem_id, 2 * sizeof(metaqueue_data), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    //point metaqueue pointer of current process to assigned memory address
    process[process_current_counter].metaqueue[0] = (metaqueue_data *) shmat(shm_id, NULL, 0);
    process[process_current_counter].metaqueue[1] = (metaqueue_data *)
            ((uint8_t *) process[process_current_counter].metaqueue[0] +
             sizeof(metaqueue_data));
    if (process[process_current_counter].metaqueue[0] == (void *) -1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    memset(process[process_current_counter].metaqueue[0], 0, 2 * sizeof(metaqueue_data));
    metaqueue_pack tmp_pack;
    tmp_pack.data = process[process_current_counter].metaqueue[0];
    tmp_pack.meta = &process[process_current_counter].metaqueue_meta[0];
    metaqueue_init_data(tmp_pack);
    metaqueue_init_meta(tmp_pack);
    tmp_pack.data = process[process_current_counter].metaqueue[1];
    tmp_pack.meta = &process[process_current_counter].metaqueue_meta[1];
    metaqueue_init_data(tmp_pack);
    metaqueue_init_meta(tmp_pack);
    key_t ret_val = process[process_current_counter].uniq_shmem_id;
    //test ping-pong
    ++process_current_counter;
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
