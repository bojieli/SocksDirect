//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/socket_lib.h"
static uint8_t buffer[1048576] __attribute__((aligned(PAGE_SIZE)));

int main(int argc, char* argv[])
{
    int warmup_num = 1000000;
    const int test_num=10000;

    int test_size;
    if (argc < 2)
        test_size = 8;
    else
        test_size = atoi(argv[2]);
    pin_thread(0);

    if (test_size >= 8192)
        warmup_num /= 10;
    if (test_size >= 131072)
        warmup_num /= 10;

    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
    if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect, %d", errno);
    printf("connect succeed\n");

    TimingInit();
    InitRdtsc();
    for (int i=0;i<warmup_num + test_num;++i)
    {
        //get time
        struct timespec s_time, e_time;
        //clock_gettime(CLOCK_REALTIME, &s_time);
        GetRdtscTime(&s_time);

        //ping
        write(fd, buffer, test_size);
        //pong
        read(fd, buffer, test_size);

        //get time
        //clock_gettime(CLOCK_REALTIME, &e_time);
        GetRdtscTime(&e_time);

        printf("lat = %lf us\n", (double)((e_time.tv_sec - s_time.tv_sec) * (double)1e6 + (e_time.tv_nsec - s_time.tv_nsec) / (double)1e3));
    }

    return 0;
}
