//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_PROCESS_H
#define IPC_DIRECT_PROCESS_H
#include <tuple>
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
    bool isRDMA;
    pid_t pid,tid;
    key_t uniq_shmem_id;
    uint64_t token;
    metaqueue_t metaqueue; //0: process send to monitor 1: monitor sent to process
    uint64_t glb_ref;
};
#define MAX_PROCESS_NUM 1024

extern std::tuple<key_t, uint64_t> process_add(pid_t pid, pid_t tid);

extern void process_add_rdma(const metaqueue_t * metaqueue, int rdma_proc_idx);

extern void process_init();

extern bool process_isexist(int qid);

metaqueue_t* process_gethandler_byqid(int qid);

extern int process_iterator_init();

extern int process_iterator_next(int prev);

void process_chk_remove();

extern pid_t process_gettid(int qid);

extern void fork_handler(metaqueue_ctl_element *req_body, int qid);

extern void recv_takeover_handler(metaqueue_ctl_element *req_body, int qid);

#endif //IPC_DIRECT_PROCESS_H
