//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "../common/helper.h"
#include "../lib/lib.h"

#define MAX_MSGSIZE (1024*1024)
uint8_t buffer[MAX_MSGSIZE];


struct thread_ctx_t
{
    char ip[40];
    int core_num;
    char padding[16];
} ;


struct thread_ctx_t per_thread_ctx[64];
pthread_t threads[64];


void* tput_msg_sender(void* p_ctx_tmp)
{
    struct thread_ctx_t *p_ctx=(struct thread_ctx_t *)p_ctx_tmp;

    pin_thread(p_ctx->core_num);
    while (1)
    {
        int fd;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) FATAL("Failed to create fd");
        struct sockaddr_in servaddr;
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8080);
        int tmp=1;
        inet_pton(AF_INET, p_ctx->ip, &servaddr.sin_addr);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
        tmp = MAX_MSGSIZE;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &tmp, sizeof(tmp));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp));
        if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
            printf("%s", strerror(errno));
            return 0;
        }
        close(fd);
    }
    return 0;
}

int main(int argc, char * argv[])
{
    if (argc < 3)
    {
        printf("Usage <ip> <number of cores>");
        return -1;
    }

    int coresum=atoi(argv[2]);
    if (coresum==0)
    {
        printf("Illegal core num");
        return -1;
    }

    int fd;

    for (int i=0;i<MAX_MSGSIZE;++i) buffer[i] = rand() % 256;

    for (int i=0;i<coresum;++i)
    {
        //sleep(1);
        int current_core_num = 2*i-i%2;
        //int current_core_num = i;
        strcpy(per_thread_ctx[current_core_num].ip, argv[1]);
        per_thread_ctx[current_core_num].core_num = current_core_num;
        pthread_create(&threads[current_core_num], NULL, tput_msg_sender, (void *)&per_thread_ctx[current_core_num]);
    }

    for (int i=0;i<coresum;++i) {
        //int current_core_num = 2*i-i%2;
        int current_core_num = i;
        pthread_join(threads[current_core_num], NULL);
    }

    return 0;
}
