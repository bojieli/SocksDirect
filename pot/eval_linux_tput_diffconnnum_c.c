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
#define WARMUP_NUM 10000

#define MAX_CONN_NUM 100000000
int fds[MAX_CONN_NUM];


int numofconn;
volatile int64_t counter;
volatile int done;
void* tput_msg_receiver(void* ptr)
{
    pin_thread(0);
    InitRdtsc();
    InitRdtsc();
    InitRdtsc();

    while (!done) {
        for (int i = 0; i < numofconn; ++i)
        {
            ++counter;
            int len=0;
            while (len < 8)
            {
                int onetimelen = recvfrom(fds[i], (void *) buffer + len, 8 - len, 0, NULL, NULL);
                if (onetimelen<0)
                {
                    printf("Wr err");
                    return 0;
                }
                len += onetimelen;
            }
        }
    }

    for (int i=0;i<numofconn;++i)
        close(fds[i]);
    return 0;
}



int main(int argc, char * argv[])
{
    if (argc < 4)
    {
        printf("Usage <output name> <IP> <numofconn>");
        return -1;
    }
    numofconn=atoi(argv[3]);
    if (numofconn == 0)
    {
        printf("Invalid msgsize");
        return -1;
    }


    TimingInit();
    InitRdtsc();
    pin_thread(6);

    for (int i=0;i<numofconn;++i)
    {
        int fd;
        fd= socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) FATAL("Failed to create fd");
        struct sockaddr_in servaddr;
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8080);
        inet_pton(AF_INET, argv[2], &servaddr.sin_addr);
        int tmp=1;
        //setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
        if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
            FATAL("Failed to connect %s", strerror(errno));
        fds[i]=fd;
    }

    done=0;
    pthread_t thread;
    counter=0;
    pthread_create(&thread, NULL, tput_msg_receiver, NULL);

    sleep(1);

    uint64_t old_ctr, n_ctr;
    struct timespec e_time, s_time;
    old_ctr = counter;
    InitRdtsc();
    GetRdtscTime(&s_time);
    while (1) {
        GetRdtscTime(&e_time);
        double duration;
        duration = (e_time.tv_sec-s_time.tv_sec) + (e_time.tv_nsec-s_time.tv_nsec)*1e-9;
        if (duration > 2) break;
    }
    n_ctr = counter;
    done=1;
    double tput;
    tput = (double)(n_ctr - old_ctr) /
            ((e_time.tv_sec - s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)*1e-9) / 1000;

    FILE *output_f = fopen(argv[1], "a");
    fprintf(output_f, "%d %.0lf\n", numofconn, tput);
    fclose(output_f);

    pthread_join(thread, NULL);
    return 0;
}
