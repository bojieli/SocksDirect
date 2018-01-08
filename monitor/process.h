//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_PROCESS_H
#define IPC_DIRECT_PROCESS_H
#include "../common/locklessqueue_n.hpp"
#ifdef __cplusplus
extern "C" {
#endif

#include "../common/helper.h"

#ifdef __cplusplus
}
#endif

#include "../common/metaqueue.h"



struct process_sturc
{
    pid_t pid,tid;
    key_t uniq_shmem_id;
    metaqueue_t metaqueue; //0: process send to monitor 1: monitor sent to process
};
#define MAX_PROCESS_NUM 1024

extern key_t process_add(pid_t pid, pid_t tid);

extern void process_init();

extern bool process_isexist(int qid);

metaqueue_t* process_gethandler_byqid(int qid);

extern int process_iterator_init();

extern int process_iterator_next(int prev);

void process_chk_remove();

#endif //IPC_DIRECT_PROCESS_H
