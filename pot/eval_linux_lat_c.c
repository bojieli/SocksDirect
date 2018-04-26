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
#define TST_NUM 10000
#define WARMUP_NUM 1000
double samples[TST_NUM];

int main(int argc, char * argv[])
{
    if (argc < 4)
    {
        printf("Usage <output name> <IP> <msgsize>");
        return -1;
    }
    int msgsize=atoi(argv[3]);
    if (msgsize == 0)
    {
        printf("Invalid msgsize");
        return -1;
    }

    pin_thread(0);
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    inet_pton(AF_INET, argv[2], &servaddr.sin_addr);
    int tmp=1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
    tmp = MAX_MSGSIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &tmp, sizeof(tmp));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp));
    if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect");
    printf("connect succeed\n");

    for (int i=0;i<MAX_MSGSIZE;++i) buffer[i] = rand() % 256;

    InitRdtsc();
    for (int i=0;i<WARMUP_NUM + TST_NUM;++i)
    {
        //get time
        struct timespec s_time, e_time;
        //clock_gettime(CLOCK_REALTIME, &s_time);
        GetRdtscTime(&s_time);

        //ping
        int len=0;
        while (len < msgsize)
        {
            int onetimelen=write(fd, (void *) buffer+len, msgsize-len);
            if (onetimelen<0)
            {
                printf("Wr err");
                return -1;
            }
            len += onetimelen;
        }

        len = 0;
        while (len < msgsize)
        {
            int onetimelen = recvfrom(fd, (void *) buffer + len, msgsize - len, 0, NULL, NULL);
            if (onetimelen<0)
            {
                printf("Wr err");
                return -1;
            }
            len += onetimelen;
        }

        //get time
        //clock_gettime(CLOCK_REALTIME, &e_time);
        GetRdtscTime(&e_time);

        if (i >= WARMUP_NUM)
            samples[i - WARMUP_NUM]= (double)((e_time.tv_sec - s_time.tv_sec) * (double)1e9 + (e_time.tv_nsec - s_time.tv_nsec));
    }

    FILE * output_file = fopen(argv[1], "w");
    for (int i=0;i<TST_NUM;++i) fprintf(output_file, "%.0lf\n", samples[i]);
    fclose(output_file);
    return 0;
}
