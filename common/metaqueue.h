//
// Created by ctyi on 11/14/17.
//

#ifndef IPC_DIRECT_METAQUEUE_H
#define IPC_DIRECT_METAQUEUE_H

#include <stdint.h>
#include <sys/types.h>
#include "locklessqueue_n.hpp"

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

typedef struct __attribute__((packed))
{
    key_t oldshmemkey;
    key_t newshmemkey;
} resp_fork_t;

typedef struct __attribute__((packed))
{
    key_t oldshmemkey;
    key_t newshmemkey[2];
} resp_push_fork_t;

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
        long test_payload;
    };
    unsigned char command;
} metaqueue_ctl_element;

class metaqueue_t
{
public:
    locklessqueue_t<metaqueue_ctl_element, 256> q[2];
    locklessqueue_t<metaqueue_ctl_element, 256> q_emergency[2];
    int get_sharememsize()
    {
        return (q[0].getmemsize() * 2 + q_emergency[1].getmemsize() * 2);
    }
    void init_memlayout(uint8_t * baseaddr, int loc) //0: use the lower part to send
    {
        q[loc].sender_init(baseaddr);
        baseaddr += q[0].getmemsize();
        q[1 - loc].receiver_init(baseaddr);
        baseaddr += q[1].getmemsize();
        q_emergency[loc].sender_init(baseaddr);
        baseaddr += q[1].getmemsize();
        q_emergency[1 - loc].receiver_init(baseaddr);
    }
    void mem_init()
    {
        q[0].init_mem();
        q[1].init_mem();
        q_emergency[0].init_mem();
        q_emergency[1].init_mem();
    }

};

#endif //IPC_DIRECT_METAQUEUE_H
