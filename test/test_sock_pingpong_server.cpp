//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include "../lib/lib_internal.h"
#include "../lib/socket_lib.h"
static uint8_t buffer[1048576] __attribute__((aligned(PAGE_SIZE)));

int main(int argc, char* argv[])
{
    int warmup_num=1000000;
    int test_num=10000;

    int test_size;
    if (argc < 2)
        test_size = 8;
    else
        test_size = atoi(argv[2]);

    if (test_size >= 8192)
        warmup_num /= 10;
    if (test_size >= 131072)
        warmup_num /= 10;

    pin_thread(2);
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

    int connect_fd = accept4(fd, NULL, NULL, 0);
    if (connect_fd == -1)
        FATAL("Failed to connect to client");
    printf("Connected\n");

    TimingInit();
    InitRdtsc();
    for (int i=0;i<warmup_num + test_num;++i)
    {
        read(connect_fd, buffer, test_size);
        write(connect_fd, buffer, test_size);
    }

    return 0;
}
