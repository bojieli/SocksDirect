//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_METAQUEUE_H
#define IPC_DIRECT_METAQUEUE_H

#include <stdint.h>
#include <sys/types.h>
#include "locklessqueue_n.hpp"
#include "rdma_struct.h"
#define MAX_METAQUEUE_SIZE 256
#define METAQUEUE_MASK ((MAX_METAQUEUE_SIZE)-1)

typedef struct __attribute__((packed))
{
    unsigned short port;
    unsigned short is_reuseaddr;
} command_sock_listen_t;

typedef struct __attribute__((packed))
{
    unsigned short port;
    int fd;
    bool isRDMA;
} command_sock_connect_t;

typedef struct __attribute__((packed))
{
    int res_code;
    int err_code;
} command_resp_t;

typedef struct __attribute__((packed))
{
    key_t shm_key;
    int fd;
    unsigned short port;
    int8_t loc;
    bool isRDMA;
} resp_sock_connect_t;

typedef struct __attribute__((packed))
{
    unsigned short port;
    int listen_fd;
} command_sock_close_t;

typedef struct __attribute__((packed))
{
    pid_t old_tid;
    uint64_t token;
} command_fork_t;

typedef struct __attribute__((packed))
{
    key_t shmem;
    int req_fd;
    int peer_fd;
} command_relay_recv_t;


//This struct response to the father process
typedef struct __attribute__((packed))
{
    key_t oldshmemkey;
    key_t newshmemkey;
} resp_fork_t;

//This response to the peer of fork
typedef struct __attribute__((packed))
{
    key_t oldshmemkey;
    key_t newshmemkey[2];
} resp_push_fork_t;

typedef struct __attribute__((packed))
{
    int len;
    int subcommand;
} long_msg_head_t;


typedef struct __attribute__((packed))
{
    union __attribute__((packed)) {
        u_char raw[13];
        command_sock_close_t req_close;
        command_sock_connect_t req_connect;
        resp_sock_connect_t resp_connect;
        command_resp_t resp_command;
        command_sock_listen_t req_listen;
        command_fork_t req_fork;
        command_relay_recv_t req_relay_recv;
        resp_fork_t resp_fork;
        resp_push_fork_t push_fork;
        long_msg_head_t long_msg_head;
        long test_payload;
    };
    unsigned char command;
} metaqueue_ctl_element;

typedef struct 
{
    key_t shm_key;
    qp_info_t qpinfo;
} metaqueue_long_msg_rdmainfo_t;

class metaqueue_t
{
public:
    locklessqueue_t<metaqueue_ctl_element, 256> q[2];
    locklessqueue_t<metaqueue_ctl_element, 256> q_emergency[2];
    static int get_sharememsize()
    {
        return (locklessqueue_t<metaqueue_ctl_element, 256>::getmemsize() * 2 +
                locklessqueue_t<metaqueue_ctl_element, 256>::getmemsize() * 2);
    }
    void init_memlayout(uint8_t * baseaddr, int loc) //0: use the lower part to send
    {
        q[loc].init(baseaddr, loc);
        baseaddr += q[loc].getmemsize();
        q[1 - loc].init(baseaddr, 1 - loc);
        baseaddr += q[1-loc].getmemsize();
        q_emergency[loc].init(baseaddr, loc);
        baseaddr += q[loc].getmemsize();
        q_emergency[1 - loc].init(baseaddr, 1 - loc);
    }
    void mem_init()
    {
        q[0].init_mem();
        q[1].init_mem();
        q_emergency[0].init_mem();
        q_emergency[1].init_mem();
    }
    void initRDMA(ibv_qp *_qp, ibv_cq * _cq, uint32_t _lkey, uint32_t _rkey, uint64_t _remote_addr, int loc)
    {
        uint64_t remote_mem_addr(_remote_addr);
        q[loc].initRDMA(_qp, _cq, _lkey, _rkey, remote_mem_addr);
        remote_mem_addr += q[loc].getmemsize();
        q[1-loc].initRDMA(_qp, _cq, _lkey, _rkey, remote_mem_addr);
        remote_mem_addr += q[1-loc].getmemsize();
        q_emergency[loc].initRDMA(_qp, _cq, _lkey, _rkey, remote_mem_addr);
        remote_mem_addr += q_emergency[loc].getmemsize();
        q_emergency[1-loc].initRDMA(_qp, _cq, _lkey, _rkey, remote_mem_addr);

    }

    /* This func need to be used with special caution because may lead to deadlock*/
    void push_longmsg(size_t len, void* addr, uint8_t subcommand)
    {
        metaqueue_ctl_element ele;
        ele.command = LONG_MSG_HEAD;
        ele.long_msg_head.len = len;
        ele.long_msg_head.subcommand = subcommand;
        while (!q[0].push_nb(ele));
        size_t len_left(len);
        void* curr_ptr=addr;
        while (len_left > 0)
        {
            ele.command = LONG_MSG;
            int this_len = len_left>13?13:len_left;
            memcpy((void *)ele.raw,curr_ptr, this_len);
            while (!q[0].push_nb(ele));
            len_left -= this_len;
        }
    }
    /*We assume head already been poped
     *
     * */
    void* pop_longmsg(size_t len)
    {
        void* addr=malloc(len);
        if (addr == nullptr)
            FATAL("Err to malloc");
        void* currptr(addr);
        size_t len_left(len);
        while (len_left > 0)
        {
            metaqueue_ctl_element ele;
            while (!q[1].pop_nb(ele));
            if (ele.command != LONG_MSG)
                FATAL("Pop long msg failed, invalid type.");
            size_t this_len = len_left>13?13:len_left;
            len_left -= this_len;
            memcpy(currptr, (void*)ele.raw,this_len);
        }
    }
};

#endif //IPC_DIRECT_METAQUEUE_H
