//
// Created by ctyi on 4/29/18.
//
#include <sys/shm.h>
#include <semaphore.h>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char * argv[])
{
    int core_sum = atoi(argv[1]);
    key_t shm_key;
    if ((shm_key = ftok("/ipcd_setup_tst", 0)) < 0) {
        printf("Failed to get the key of shared memory, errno: %d", errno);
        return -1;
    }
    int shm_id = shmget(shm_key, sizeof(sem_t), IPC_CREAT | 0777);
    if (shm_id == -1) {
        printf("Failed to open the shared memory, errno: %s", strerror(errno));
        return -1;
    }
    sem_t *p_sem = (sem_t *) shmat(shm_id, NULL, 0);
    sem_init(p_sem, 1, 0);

    sleep(5);
    for (int i=0;i<core_sum;++i) if(sem_post(p_sem)!=0) printf("Error\n");
    return 0;
}