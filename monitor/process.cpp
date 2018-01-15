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
#include <sys/stat.h>

static darray_t<process_sturc, MAX_PROCESS_NUM> process;

void process_init()
{
    process.init();
}

key_t process_add(pid_t pid, pid_t tid)
{
    process_sturc curr_proc;
    int id=process.add(curr_proc);
    curr_proc.tid = tid;
    curr_proc.pid = pid;
    if ((curr_proc.uniq_shmem_id = ftok(SHM_NAME, id + 2)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    int shm_id = shmget(curr_proc.uniq_shmem_id, (size_t)process[id].metaqueue.get_sharememsize(), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    //point metaqueue pointer of current process to assigned memory address
    uint8_t* baseaddr = (uint8_t *) shmat(shm_id, NULL, 0);
    if (baseaddr == (uint8_t *)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    curr_proc.metaqueue.init_memlayout(baseaddr, 0);
    curr_proc.metaqueue.mem_init();
    key_t ret_val = curr_proc.uniq_shmem_id;
    process[id]=curr_proc;
    return ret_val;
}

metaqueue_t * process_gethandler_byqid(int qid)
{
    return  &process[qid].metaqueue;
}


int process_iterator_init()
{
    return process.iterator_init();
}

int process_iterator_next(int prev)
{
    return process.iterator_next(prev);
}

void process_del(int qid)
{
    process.del(qid);
}

#undef DEBUGON
#define DEBUGON 1
void process_chk_remove()
{
    const char* basestr="/proc/%d/task/%d/";
    char dirstr[100];
    for (int i=process_iterator_init();i!=-1;i=process_iterator_next(i))
    {
        sprintf(dirstr, basestr, process[i].pid, process[i].tid);
        struct stat sb;
        if (!(stat(dirstr, &sb) == 0 && S_ISDIR(sb.st_mode)))
        {
            DEBUG("thread %d in process %d not exits", process[i].tid, process[i].pid);
            process_del(i);
        }
    }
}

bool process_isexist(int qid)
{
    return process.isvalid(qid);
}

pid_t process_gettid(int qid)
{
    return process[qid].tid;
}
#undef DEBUGON