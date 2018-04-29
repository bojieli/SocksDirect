//
// Created by ctyi on 11/30/17.
//


#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include <semaphore.h>
#include <sys/shm.h>


#define MAX_MSGSIZE (1024*1024)

struct thread_ctx_t
{
    int fd;
    int core_num;
    int msgsize;
    int counter;
    int ready;
    int padding[64 / sizeof(int) - 5];
} ;

#define WARMUP_RND 100000
#define TST_RND 100000
int done[64];
pthread_t threads[64];
pid_t pid[64];
int pipefds[64][2];




int main(int argc, char * argv[])
{
    int shm_id = shmget(0, sizeof(sem_t), IPC_CREAT | 0777);
    if (shm_id == -1)
        FATAL("Failed to open the shared memory, errno: %s", strerror(errno));
    //point metaqueue pointer of current process to assigned memory address
    sem_t* sem_p = (sem_t *) shmat(shm_id, NULL, 0);
    if (argc < 2)
    {
        printf("Usage: <number of cores>");
        return -1;
    }
    int core_sum=atoi(argv[1]);
    if (core_sum == 0)
    {
        printf("Illegal core num");
        return -1;
    }

    TimingInit();

    if (sem_init(sem_p, 1, 0) < 0)
    {
        printf("Init semaphore failed\n");
        return -1;
    }


    for (int i=0;i<core_sum;++i) {
        int current_core_num = i* 2 + 2 - i % 2;
        pid_t fork_ret;
        pipe(pipefds[current_core_num]);
        fork_ret = fork();
        if (fork_ret != 0) //This is the parent process
        {
            pid[current_core_num] = fork_ret;
        }
        else { //This is the child process
            pin_thread(current_core_num);

            int fd;
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd == -1) FATAL("Failed to create fd");
            int property=1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &property, sizeof(int));

            struct sockaddr_in servaddr;
            servaddr.sin_family = AF_INET;
            servaddr.sin_port = htons(8080);
            servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
                FATAL("failed to bind %s", strerror(errno));
            if (listen(fd, 100000) == -1)
                FATAL("listen failed");

            printf("Listen succeed on core %d\n", current_core_num);


            for (int j=0;j<WARMUP_RND;++j)
            {
                int connect_fd = accept4(fd, NULL, NULL, 0);
                //printf("Haha\n");
                close(connect_fd);
            }
            InitRdtsc();
            sem_wait(sem_p);
            printf("Started\n");
            struct timespec s_time, e_time;
            GetRdtscTime(&s_time);
            for (int j=0;j<TST_RND;++j) {
                int connect_fd = accept4(fd, NULL, NULL, 0);
                close(connect_fd);
            }
            GetRdtscTime(&e_time);
            double tput;
            tput = (TST_RND)/ (double)((e_time.tv_sec - s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)/(double)1e9) / (double)1000;
            write(pipefds[current_core_num][1], (void *)&tput, sizeof(double));
            printf("Tput for core %d is %.0lfkop/s\n",
                current_core_num, tput
                );
            return 0;
        }
    }
    //sleep(5);
    for (int i=0;i<core_sum;++i) if(sem_post(sem_p)!=0) printf("Error\n");
    printf("Post\n");
    double total_tput(0);
    for (int i=0;i<core_sum;++i)
    {
        int current_core_num = i* 2 + 2 - i % 2;
        double tput(0);
        read(pipefds[current_core_num][0], (void *)&tput, sizeof(double));
        total_tput += tput;

    }
    FILE * output_f;
    output_f = fopen("tput.out", "a");
    fprintf(output_f, "%d %.0lf\n", core_sum, total_tput);
    fclose(output_f);
    for (int i=0;i<core_sum;++i) wait(NULL);

    return 0;
}
