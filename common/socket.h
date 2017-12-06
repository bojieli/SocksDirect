//
// Created by ctyi on 11/24/17.
//

#ifndef IPC_DIRECT_SOCKET_H
#define IPC_DIRECT_SOCKET_H
typedef struct
{
    int isvalid;
    int fd_to;
    void *data;
    int command;
} sock_element_t;
typedef struct
{
    int pointer;
    sock_element_t data[256];
} sock_queue_t;
#endif //IPC_DIRECT_SOCKET_H
