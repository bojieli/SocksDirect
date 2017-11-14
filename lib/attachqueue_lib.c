//
// Created by ctyi on 11/14/17.
//
#include "../common/helper.h"
#include "../common/attachqueue.h"
#include "attachqueue_lib.h"
static sem_t *sem_fill_count_req=NULL,
        *sem_empty_count_req=NULL,*sem_mutex_req=NULL;

static sem_t *sem_fill_count_ack=NULL,
        *sem_empty_count_ack=NULL,*sem_mutex_ack=NULL;
aqueue_struc *attachqueue_req=NULL, *attachqueue_ack=NULL;
static key_t shm_key;
static int shm_id;
static void* base_addr;
void attachqueue_init()
{
    //init the shared memory
    if ((shm_key = ftok(SHM_NAME, 1)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    shm_id = shmget(shm_key, 2*sizeof(aqueue_struc), 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    base_addr = shmat(shm_id, NULL, 0);
    if (base_addr == (void*)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    attachqueue_req = base_addr;
    attachqueue_ack = base_addr + sizeof(aqueue_struc);
    //init the semaphore
    if (attachqueue_req == NULL || attachqueue_ack==NULL)
        FATAL("Failed to get the address of attachqueue, maybe forget to call addrinit.");
    sem_fill_count_req = sem_open(SEM_FILL_COUNT_REQ, 0);
    if (sem_fill_count_req == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));

    sem_empty_count_req = sem_open(SEM_EMPTY_COUNT_REQ, 0);
    if (sem_empty_count_req == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));

    sem_mutex_req = sem_open(SEM_EMPTY_COUNT_REQ, 0);
    if (sem_mutex_req == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));

    sem_fill_count_ack = sem_open(SEM_FILL_COUNT_ACK, 0);
    if (sem_fill_count_ack == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));

    sem_empty_count_ack = sem_open(SEM_EMPTY_COUNT_ACK, 0);
    if (sem_empty_count_ack == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));

    sem_mutex_ack = sem_open(SEM_EMPTY_COUNT_ACK, 0);
    if (sem_mutex_ack == SEM_FAILED)
        FATAL("semaphore open failed on Line %d. %s",
              __LINE__, strerror(errno));
}


ctl_struc attachqueue_pullack(pid_t pid){
    ctl_struc result;
    while (attachqueue_ack->data[attachqueue_ack->tail].pid != pid)
            SW_BARRIER;
    sem_wait(sem_fill_count_ack);
    sem_wait(sem_mutex_ack);
    result=attachqueue_ack->data[attachqueue_ack->tail];
    attachqueue_ack->tail++;
    sem_post(sem_mutex_ack);
    sem_post(sem_empty_count_ack);
    return result;
}
ctl_struc attachqueue_pushreq(ctl_struc data){
    sem_wait(sem_empty_count_req);
    sem_wait(sem_mutex_req);
    attachqueue_req->data[attachqueue_req->head]=data;
    attachqueue_req->head++;
    sem_post(sem_mutex_req);
    sem_post(sem_fill_count_req);
}