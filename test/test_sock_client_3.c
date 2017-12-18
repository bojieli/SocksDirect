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
    for (int i = 0; i < 1024; ++i) buffer[i] = (uint8_t) (i % 256);

    int counter=0;
    while (1)
    {
        for (int i=0;i<FD_NUM;++i)
        {
            fds[i] = socket(AF_INET, SOCK_STREAM, 0);
            if (fds[i] == -1)
                FATAL("Failed to create FD");
            if (connect(fds[i], (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
                FATAL("Failed to connect");
        }
        printf("1\n");
        for (int i=0;i<FD_NUM;++i)
        {
            *(int *)buffer = i;
            if (writev(fds[i], &iovec1, 1) == -1)
                FATAL("write error");
            printf("write %d\n",i);
        }
        printf("2\n");
        for (int i=0;i<FD_NUM;++i)
            if (close(fds[i]) != 0)
                FATAL("Failed to close the socket");
        ++counter;
        if (counter % 100 == 0)
            printf("counter:%d * 1024 * 3\n", counter);
    }
    return 0;
}
