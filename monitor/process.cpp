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
#include <unordered_map>
#include <cstdlib>
#include "sock_monitor.h"
#include "../lib/rdma_lib.h"

static darray_t<process_sturc, MAX_PROCESS_NUM> process;
static std::unordered_map<pid_t, int> tidmap;

void process_init()
{
    process.init();
}

std::tuple<key_t, uint64_t> process_add(pid_t pid, pid_t tid)
{
    process_sturc curr_proc;
    int id=process.add(curr_proc);
    curr_proc.tid = tid;
    curr_proc.pid = pid;
    curr_proc.isRDMA=false;
    curr_proc.token = (uint64_t)rand();

    if (access(SHM_NAME, F_OK) == -1) {
        DEBUG("Shared memory token %s does not exist, trying to create", SHM_NAME);
        if (creat(SHM_NAME, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == -1) {
            FATAL("Failed to create shared memory token file %s, errno: %d (%s)", SHM_NAME, errno, strerror(errno));
        }
    }
    if ((curr_proc.uniq_shmem_id = ftok(SHM_NAME, id + 2)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d (%s)", errno, strerror(errno));
    int shm_id = shmget(curr_proc.uniq_shmem_id, (size_t)process[id].metaqueue.get_sharememsize(), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %d (%s)", errno, strerror(errno));
    //point metaqueue pointer of current process to assigned memory address
    uint8_t* baseaddr = (uint8_t *) shmat(shm_id, NULL, 0);
    if (baseaddr == (uint8_t *)-1)
        FATAL("Failed to attach the shared memory, errno: %d (%s)", errno, strerror(errno));
    curr_proc.metaqueue.init_memlayout(baseaddr, 0);
    curr_proc.metaqueue.mem_init();
    key_t ret_val = curr_proc.uniq_shmem_id;
    process[id]=curr_proc;
    tidmap[tid] = id;
    return std::make_tuple(ret_val, curr_proc.token);
}

void process_add_rdma(const metaqueue_t * metaqueue, int rdma_proc_idx)
{
    process_sturc curr_proc;
    curr_proc.metaqueue = *metaqueue;
    curr_proc.isRDMA = true;
    curr_proc.glb_ref = (uint64_t)(1<<64) + rdma_proc_idx;
    int id=process.add(curr_proc);
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
#define DEBUGON 0
void process_chk_remove()
{
    const char* basestr="/proc/%d/task/%d/";
    char dirstr[100];
    for (int i=process_iterator_init();i!=-1;i=process_iterator_next(i))
    {
        //If RDMA, I cannot check
        //TODO: RDMA dead check
        if (process[i].isRDMA) continue;
        sprintf(dirstr, basestr, process[i].pid, process[i].tid);
        struct stat sb;
        if (!(stat(dirstr, &sb) == 0 && S_ISDIR(sb.st_mode)))
        {
            DEBUG("thread %d in process %d not exits", process[i].tid, process[i].pid);
            tidmap.erase(process[i].tid);
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
#define DEBUGON 0

void fork_handler(metaqueue_ctl_element *req_body, int qid)
{
    //Auth
    int old_tid = req_body->req_fork.old_tid;
    int new_tid = process[qid].tid;
    metaqueue_ctl_element res_err;
    res_err.command = RES_ERROR;
    res_err.resp_command.err_code = 1;
    if (tidmap.find(old_tid) == tidmap.end())
    {
        process[qid].metaqueue.q[0].push(res_err);
        return;
    }
    if (process[tidmap[old_tid]].token != req_body->req_fork.token)
    {
        process[qid].metaqueue.q[0].push(res_err);
        return;
    }

    const per_proc_map_t* old_tid_hash = buff_get_handler(old_tid);
    //old tid does not have any buffer
    if (old_tid_hash == nullptr)
    {
        metaqueue_ctl_element resp;
        resp.command = FIN_FORK;
        process_gethandler_byqid(qid)->q[0].push(resp);
        return;
    }
    //iterate all the previous buffer
    for (auto iter_peer = old_tid_hash->begin(); iter_peer != old_tid_hash->end(); iter_peer++)
    {
        int peer_tid = iter_peer->first;
        for (auto iter_key = iter_peer->second.begin();iter_key != iter_peer->second.end();++iter_key) {
            interprocess_buf_map_t old_buffer_info = *iter_key;
            //Delete the old one
            //buffer_del(old_tid, peer_tid);
            //Create the new buffer
            key_t shm_key4nproc = buffer_new(new_tid, peer_tid, old_buffer_info.loc);
            key_t shm_key4oldproc = buffer_new(old_tid, peer_tid, old_buffer_info.loc);
            metaqueue_ctl_element resp4nproc, resp4oldproc, resp4peerproc;
            resp4nproc.command = RES_FORK;
            resp4nproc.resp_fork.oldshmemkey = old_buffer_info.buffer_key;
            resp4nproc.resp_fork.newshmemkey = shm_key4nproc;
            process_gethandler_byqid(qid)->q[0].push(resp4nproc);
            resp4oldproc.command = RES_FORK;
            resp4oldproc.resp_fork.oldshmemkey = old_buffer_info.buffer_key;
            resp4oldproc.resp_fork.newshmemkey = shm_key4oldproc;
            process_gethandler_byqid(tidmap[old_tid])->q[0].push(resp4oldproc);
            resp4peerproc.command = RES_PUSH_FORK;
            resp4peerproc.push_fork.oldshmemkey = old_buffer_info.buffer_key;
            resp4peerproc.push_fork.newshmemkey[0] = shm_key4oldproc;
            resp4peerproc.push_fork.newshmemkey[1] = shm_key4nproc;
            process_gethandler_byqid(tidmap[peer_tid])->q_emergency[0].push(resp4peerproc);
        }
    }
    //send FIN to all the queues
    metaqueue_ctl_element resp_fin;
    resp_fin.command = FIN_FORK;
    //send to new fork process
    process_gethandler_byqid(qid)->q[0].push(resp_fin);
    //send to old fork process
    process_gethandler_byqid(tidmap[old_tid])->q[0].push(resp_fin);
}

#undef DEBUGON
#define DEBUGON 1

void recv_takeover_handler(metaqueue_ctl_element *req_body, int qid)
{
    key_t shmem_key = req_body->req_relay_recv.shmem;
    if (interprocess_key2tid.find(shmem_key) == interprocess_key2tid.end())
        FATAL("Takeover message cannot forward since key %u not find", shmem_key);
    std::pair<pid_t, pid_t> tid_pair = interprocess_key2tid[shmem_key];
    int dest_qid;
    //itself is the first one
    if (process[qid].tid == tid_pair.first)
        dest_qid = tidmap[tid_pair.second];
    else
        dest_qid = tidmap[tid_pair.first];
    if (req_body->command == REQ_RELAY_RECV)
        DEBUG("relay takeover message for key %u from %d to %d", shmem_key, qid, dest_qid);
    else
        DEBUG("relay takeover ack message for key %u from %d to %d", shmem_key, qid, dest_qid);
    process[dest_qid].metaqueue.q_emergency[0].push(*req_body);
}

void long_msg_handler(metaqueue_ctl_element * req_body, int qid)
{
    int len=req_body->long_msg_head.len;
    int subcommand = req_body->long_msg_head.subcommand;
    if (subcommand != RDMA_QP_INFO)
        FATAL("Invalid subcommand");
    metaqueue_t * req_metaqueue(nullptr), * res_metaqueue(nullptr);
    req_metaqueue = &process[qid].metaqueue;
    metaqueue_long_msg_rdmainfo_t* rdma_send_info_ptr =  (metaqueue_long_msg_rdmainfo_t *)req_metaqueue->pop_longmsg(len);
    int key = rdma_send_info_ptr->shm_key;
    //We need to find which process is map to the key
    if (rdma_key2qid.find(key) == rdma_key2qid.end())
        FATAL("Failed to find the shm key");
    int qid1,qid2, peerqid;
    std::tie(qid1, qid2) = rdma_key2qid[key];
    if (process[qid].isRDMA)
                peerqid = qid1;
    else
        peerqid = qid2;

    if (peerqid == qid)
        FATAL("Failed to find peer qid");
    //Find the metaqueue of the peerqid
    res_metaqueue = &process[peerqid].metaqueue;
    res_metaqueue->push_longmsg(len,rdma_send_info_ptr, RDMA_QP_INFO);
    DEBUG("Monitor relay QPinfo to queue %d finished!", peerqid);
    free(rdma_send_info_ptr);
}

bool process_isRDMA(int qid)
{
    return process[qid].isRDMA;
}

#undef DEBUGON
