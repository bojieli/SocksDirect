//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include "../lib/lib_internal.h"
#include "../lib/socket_lib.h"
#include "../lib/pot_socket_lib.h"
uint8_t buffer[1048576]  __attribute__((aligned(PAGE_SIZE)));

int main(int argc, char* argv[])
{
    int test_size = 8;
    if (argc >= 2)
        test_size = atoi(argv[1]);

    int thread = 0;
    if (argc >= 4)
        thread = atoi(argv[3]);

    pin_thread(thread);

    int conn_num = 1;
    if (argc >= 5)
        conn_num = atoi(argv[4]);

    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    int property=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &property, sizeof(int));
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080 + thread);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind %s", strerror(errno));
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");

    int test_num = 100000000;
    if (test_size >= 512)
        test_num /= 10;
    if (test_size >= 131072)
        test_num /= 10;

    pot_init_write();
    TimingInit();
    InitRdtsc();

    int *connect_fd = (int *) malloc(sizeof(int) * conn_num);
    if (connect_fd == NULL) {
        printf("failed to alloc mem\n");
        return 1;
    }
    for (int i=0; i<conn_num; i++) {
        connect_fd[i] = pot_accept4(fd, NULL, NULL, 0);
        if (connect_fd[i] == -1)
            FATAL("Failed to connect to client");
    }
    printf("Connected\n");

    // warmup
    for (int i=0;i<test_num;++i)
    {
        pot_rdma_read_nbyte(connect_fd[i % conn_num], test_size);
    }

    struct timespec e_time, s_time;
    GetRdtscTime(&s_time);
    for (int i=0;i<test_num;++i)
    {
        pot_rdma_read_nbyte(connect_fd[i % conn_num], test_size);
    }
    GetRdtscTime(&e_time);

    FILE *fp = stdout;
    if (argc >= 3) {
        fp = fopen(argv[2], "a");
        if (fp == NULL)
            printf("Failed to open log file %s\n", argv[2]);
    }
    fprintf(fp, "%d\t%.0lf\n", conn_num, (double)test_num / ((e_time.tv_sec-s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)/1e9) / 1e3);
    fclose(fp);
}
