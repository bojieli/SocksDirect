#include "../common/helper.h"
#include "../common/attachqueue.h"
static sem_t *sem_fill_count_req=NULL,
        *sem_empty_count_req=NULL,*sem_mutex_req=NULL;

static sem_t *sem_fill_count_ack=NULL,
        *sem_empty_count_ack=NULL,*sem_mutex_ack=NULL;

aqueue_struc *attachqueue_req=NULL, *attachqueue_ack=NULL;
static key_t shm_key;
static int shm_id;
static void* base_addr;
void attachqueue_sysinit()
{
    if ((shm_key = ftok(SHM_NAME, 1)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    shm_id = shmget(shm_key, 2*sizeof(aqueue_struc), IPC_CREAT|0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %d", errno);
    base_addr = shmat(shm_id, NULL, 0);
    if (base_addr == (void*)-1)
        FATAL("Failed to attach the shared memory, err: %s", strerror(errno));
    //allocate the two pointer of the two queue
    attachqueue_req = base_addr;
    attachqueue_ack = base_addr + sizeof(aqueue_struc);
    //init the data of the queue
    attachqueue_req->head = attachqueue_req->tail = 0;
    attachqueue_ack->head = attachqueue_ack->tail = 0;
    //create semaphores
    if (attachqueue_req == NULL || attachqueue_ack==NULL)
        FATAL("Failed to get the address of attachqueue, maybe forget to call addrinit.");
    sem_unlink(SEM_EMPTY_COUNT_ACK);
    sem_unlink(SEM_FILL_COUNT_ACK);
    sem_unlink(SEM_MUTEX_ACK);
    sem_unlink(SEM_EMPTY_COUNT_REQ);
    sem_unlink(SEM_FILL_COUNT_REQ);
    sem_unlink(SEM_MUTEX_REQ);
    sem_fill_count_req = sem_open(SEM_FILL_COUNT_REQ, O_CREAT|O_EXCL, 0777, 0);
    if (sem_fill_count_req == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));

    sem_empty_count_req = sem_open(SEM_EMPTY_COUNT_REQ, O_CREAT|O_EXCL, 0777, MQUEUE_SIZE);
    if (sem_empty_count_req == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));

    sem_mutex_req = sem_open(SEM_MUTEX_REQ, O_CREAT|O_EXCL, 0777, 1);
    if (sem_mutex_req == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));

    sem_fill_count_ack = sem_open(SEM_FILL_COUNT_ACK, O_CREAT|O_EXCL, 0777, 0);
    if (sem_fill_count_ack == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));

    sem_empty_count_ack = sem_open(SEM_EMPTY_COUNT_ACK, O_CREAT|O_EXCL, 0777, MQUEUE_SIZE);
    if (sem_empty_count_ack == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));

    sem_mutex_ack = sem_open(SEM_MUTEX_ACK, O_CREAT|O_EXCL, 0777, 1);
    if (sem_mutex_ack == SEM_FAILED)
        FATAL("semaphore create failed. %s", strerror(errno));
    DEBUG("Queue successfully initalized");
}

int attachqueue_isempty()
{
    int count;
    if (sem_getvalue(sem_fill_count_req, &count) == -1)
        FATAL("Get semaphore error");
    if (count == 0) return 1;
    else return 0;
}

void attachqueue_pullreq(ctl_struc *result)
{
    if (result == NULL) FATAL("No space to pull process info");
    sem_wait(sem_fill_count_req);
    sem_wait(sem_mutex_req);
    //printf("req queue head %hu tail %hu\n",
    //       mqueue_req->head, mqueue_req->tail);
    *result=attachqueue_req->data[attachqueue_req->tail];
    attachqueue_req->tail++;
    sem_post(sem_mutex_req);
    sem_post(sem_empty_count_req);
}
void attachqueue_pushack(ctl_struc *result)
{
    sem_wait(sem_empty_count_ack);
    sem_wait(sem_mutex_ack);
    attachqueue_ack->data[attachqueue_ack->head]=*result;
    attachqueue_ack->head++;
    sem_post(sem_mutex_ack);
    sem_post(sem_fill_count_ack);
}


