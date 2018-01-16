//
// Created by ctyi on 11/20/17.
//

#ifndef IPC_DIRECT_SETUP_SOCK_H
#define IPC_DIRECT_SETUP_SOCK_H
typedef struct
{
    pid_t tid;
    pid_t pid;
    key_t key;
    uint64_t token;
} ctl_struc;
#endif //IPC_DIRECT_SETUP_SOCK_H
