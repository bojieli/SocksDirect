//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/pot_socket_lib.h"
#include <sys/shm.h>
#include <sys/types.h>
#include <semaphore.h>
uint8_t buffer[1048576]  __attribute__((aligned(PAGE_SIZE)));
int main(int argc, char* argv[])
{
    const int warmup_num = 10000;
    const int test_num=1000000;

    int core_num=atoi(argv[1]);
    pin_thread(core_num);

    key_t shm_key;
    if ((shm_key = ftok("/ipcd_setup_tst", 0)) < 0)
        FATAL("Failed to get the key of shared memory, errno: %d", errno);
    int shm_id = shmget(shm_key, sizeof(sem_t), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    sem_t *p_sem = (sem_t *) shmat(shm_id, NULL, 0);


    pot_init_write();
    TimingInit();
    InitRdtsc();
    for (int i=0;i<warmup_num; ++i)
    {
        int fd;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) FATAL("Failed to create fd");
        struct sockaddr_in servaddr;
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
        if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
            FATAL("Failed to connect, %d", errno);
        //printf("connect succeed\n");
        //close(fd);
    }
    printf("Heated!\n");

    while (sem_trywait(p_sem) != 0);
    struct timespec s_time, e_time;
    GetRdtscTime(&s_time);

    for (int i=0;i<test_num;++i)
    {
        int fd;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) FATAL("Failed to create fd");
        struct sockaddr_in servaddr;
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
        if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
            FATAL("Failed to connect, %d", errno);
        //printf("connect succeed\n");
        //close(fd);
    }

    GetRdtscTime(&e_time);
    double tput = test_num / ((double)((e_time.tv_sec - s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec) * 1e-9)) / 1000;
    FILE *out_f;
    out_f=fopen("setup.out", "a");
    fprintf(out_f, "%.0lf\n", tput);
    fclose(out_f);

    return 0;
}
