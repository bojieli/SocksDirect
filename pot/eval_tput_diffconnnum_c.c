//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/pot_socket_lib.h"
uint8_t buffer[1048576]  __attribute__((aligned(PAGE_SIZE)));

int main(int argc, char* argv[])
{
    int test_size = 8;
    if (argc == 2)
        test_size = atoi(argv[1]);

    int thread = 0;
    if (argc >= 4)
        thread = atoi(argv[3]);

    pin_thread((4 * thread) % 31);

    int conn_num = 1;
    if (argc >= 5)
        conn_num = atoi(argv[4]);

    int *fd = (int *) malloc(sizeof(int) * conn_num);
    if (fd == NULL) {
        printf("failed to alloc mem\n");
        return 1;
    }
    for (int i=0; i<conn_num; i++) {
        fd[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fd[i] == -1) FATAL("Failed to create fd");
        struct sockaddr_in servaddr;
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(8080 + thread);
        inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
        if (connect(fd[i], (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
            FATAL("Failed to connect, %d", errno);
    }
    printf("connect succeed\n");

    int test_num = 100000000;
    if (test_size >= 512)
        test_num /= 10;
    if (test_size >= 131072)
        test_num /= 10;

    pot_init_write();
    // warmup
    for (int i=0;i<test_num;++i)
    {
        pot_write_nbyte(fd[i % conn_num], test_size);
    }

    TimingInit();
    InitRdtsc();
    for (int i=0;i<test_num;++i)
    {
        pot_write_nbyte(fd[i % conn_num], test_size);
    }
    return 0;
}
