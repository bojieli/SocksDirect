//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/lib.h"

int main()
{
    const int FD_NUM=128;
    int fds[FD_NUM];
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    uint8_t buffer[1024];
    struct iovec iovec1;
    iovec1.iov_len = 1024;
    iovec1.iov_base = (void *) &buffer;
    int counter=0;
    int init_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(init_fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect");
    close(init_fd);
    while (1)
    {
        int fd=socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1)
            FATAL("fail to create fd");
        if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
            FATAL("Failed to connect");
        if (close(fd) != 0)
            FATAL("fail to close");
        ++counter;
        if (counter % 1000 == 0)
            printf("1k sent\n");
        printf("send 1\n");
    }
    return 0;
}
