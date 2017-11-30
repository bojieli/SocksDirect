//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_PROCESS_H
#define IPC_DIRECT_PROCESS_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../common/helper.h"
#include "../common/metaqueue.h"

typedef struct {
    pid_t pid;
    key_t uniq_shmem_id;
    metaqueue_data *metaqueue[2]; //0: process send to monitor 1: monitor sent to process
    metaqueue_meta_t metaqueue_meta[2];
} process_sturc;
#define MAX_PROCESS_NUM 1024

extern key_t process_add(pid_t pid);

extern void process_init();

extern int process_current_counter;

metaqueue_pack process_getresponsehandler_byqid(int qid);

metaqueue_pack process_getrequesthandler_byqid(int qid);

#ifdef __cplusplus
}
#endif
#endif //IPC_DIRECT_PROCESS_H
