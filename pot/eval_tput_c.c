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

    pin_thread(0);

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

    int test_num = 100000000;
    if (test_size >= 512)
        test_num /= 10;
    if (test_size >= 131072)
        test_num /= 10;

    pot_init_write();
    // warmup
    for (int i=0;i<test_num;++i)
    {
        pot_write_nbyte(fd, test_size);
    }

    TimingInit();
    InitRdtsc();
    for (int i=0;i<test_num;++i)
    {
        pot_write_nbyte(fd, test_size);
    }
    return 0;
}
