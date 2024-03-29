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
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
    if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect");
    printf("connect succeed\n");
    uint8_t buffer[1024];
    struct iovec iovec1;
    iovec1.iov_len = 16;
    iovec1.iov_base = (void *) &buffer;
    for (int i = 0; i < 1024; ++i) buffer[i] = (uint8_t) (i % 256);
    unsigned int counter;
    counter=0;
    //writev(fd, &iovec1, 1);
    for (int i=0;i<=10;++i)
    {
        *(int *)buffer = counter;
        ++counter;
        if (writev(fd, &iovec1, 1) == -1)
            FATAL("write failed");
    }
    close(fd);
    return 0;
}
