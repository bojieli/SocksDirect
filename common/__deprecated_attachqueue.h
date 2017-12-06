//
// Created by ctyi on 11/10/17.
//

#ifndef IPC_DIRECT_ATTACHQUEUE_H
#define IPC_DIRECT_ATTACHQUEUE_H

#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <semaphore.h>

typedef struct
{
    pid_t pid;
    key_t key;
} ctl_struc;

typedef struct
{
    uint8_t head, tail;
    ctl_struc data[256];

} aqueue_struc;
//some configuration for shared queue to attach process
#define MQUEUE_SIZE 256
#define SEM_EMPTY_COUNT_REQ "/ipcd_empty_count_req"
#define SEM_FILL_COUNT_REQ "/ipcd_full_count_req"
#define SEM_MUTEX_REQ "/ipcd_mutex_req"

#define SEM_EMPTY_COUNT_ACK "/ipcd_empty_count_ack"
#define SEM_FILL_COUNT_ACK "/ipcd_full_count_ack"
#define SEM_MUTEX_ACK "/ipcd_mutex_ack"
#endif //IPC_DIRECT_ATTACHQUEUE_H
