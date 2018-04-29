//
// Created by ctyi on 11/30/17.
//


#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../common/helper.h"
#include "../lib/lib.h"



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
int done[64];
struct thread_ctx_t per_thread_ctx[64];
pthread_t threads[64];


uint8_t buffer[MAX_MSGSIZE];

void* tput_msg_receiver(void* p_ctx_tmp)
{
    struct thread_ctx_t * p_ctx = (struct thread_ctx_t *)p_ctx_tmp;
    int fd = p_ctx->fd;
    int corenum = p_ctx->core_num;
    int msgsize = p_ctx->msgsize;
    pin_thread(corenum);
    InitRdtsc();
    printf("Connection for core %d inited\n", corenum);
    for (int i=0;i<WARMUP_RND;++i)
    {
        int len = 0;
        while (len < msgsize)
            len += recvfrom(fd, (void *) buffer+len, msgsize-len, 0, NULL, NULL);
    }
    p_ctx->ready = 1;
    while (!done[corenum])
    {
        int len = 0;
        while (len < msgsize)
            len += recvfrom(fd, (void *) buffer+len, msgsize-len, 0, NULL, NULL);
        ++p_ctx->counter;
    }
    close(fd);
    return 0;
}
int main(int argc, char * argv[])
{
    if (argc < 3)
    {
        printf("Usage: <msgsize> <number of cores>");
        return -1;
    }

    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    int property=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &property, sizeof(int));

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind %s", strerror(errno));
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");
    int msgsize=atoi(argv[1]);
    if (msgsize== 0)
    {
        printf("Invalid msg size");
        return -1;
    }
    int core_sum=atoi(argv[2]);
    if (core_sum == 0)
    {
        printf("Illegal core num");
        return -1;
    }

    TimingInit();

    for (int i=0;i<core_sum;++i)
    {
        int current_core_num = i* 2 + 2 - i % 2;
        int connect_fd = accept4(fd, NULL, NULL, 0);
        if (connect_fd == -1)
            FATAL("Failed to connect to client %s", strerror(errno));
        int tmp = 1;
        setsockopt( connect_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
        tmp = MAX_MSGSIZE;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &tmp, sizeof(tmp));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp));

        per_thread_ctx[current_core_num].counter = 0;
        per_thread_ctx[current_core_num].msgsize = msgsize;
        per_thread_ctx[current_core_num].core_num = current_core_num;
        per_thread_ctx[current_core_num].fd = connect_fd;
        per_thread_ctx[current_core_num].ready = 0;
        done[current_core_num] = 0;
        pthread_create(&threads[current_core_num],NULL, tput_msg_receiver, &per_thread_ctx[current_core_num]);
    }




    bool allready(true);
    do
    {
        allready=true;
        for (int i=0;i<core_sum;++i)
        {
            int current_core_num = i* 2 + 2 - i % 2;
            allready = allready && per_thread_ctx[current_core_num].ready;
        }
    }while (!allready);
    InitRdtsc();

    int64_t old_ctr(0), new_ctr(0);
    FILE* output_f;
    output_f=fopen("data.out","a");
    fprintf(output_f, "%d ", core_sum);
    for (int i=0;i<60;++i)
    {
        old_ctr = new_ctr = 0;
        for (int j=0;j<core_sum;++j)
        {
            int current_core_num = j* 2 + 2 - j % 2;
            old_ctr += per_thread_ctx[current_core_num].counter;
        }
        struct timespec s_time, e_time;
        GetRdtscTime(&s_time);
        sleep(1);
        GetRdtscTime(&e_time);
        for (int j=0;j<core_sum;++j)
        {
            int current_core_num = j* 2 + 2 - j % 2;
            new_ctr += per_thread_ctx[current_core_num].counter;
        }
        fprintf(output_f, "%.0lf ", (new_ctr-old_ctr)/ (double)((e_time.tv_sec - s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)/(double)1e9) / (double)1000);
    }
    fprintf(output_f, "\n");
    fclose(output_f);
    //fprintf(stderr, "Tput: %.0lf kop/s\n", (new_ctr-old_ctr)/ (double)((e_time.tv_sec - s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)/(double)1e9) / (double)1000);
    for (int i=0;i<core_sum;++i)
    {
        int current_core_num = i* 2 + 2 - i % 2;
        done[current_core_num]=1;
    }
    for (int i=0;i<core_sum;++i)
    {
        int current_core_num = i* 2 + 2 - i % 2;
        pthread_join(threads[current_core_num], NULL);
    }
    return 0;
}
