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
    iovec1.iov_len = 1024;
    iovec1.iov_base = (void *) &buffer;
    for (int i = 0; i < 1024; ++i) buffer[i] = (uint8_t) (i % 256);
    uint8_t counter;
    counter=0;
    //writev(fd, &iovec1, 1);
    while (1)
    {
        buffer[0] = counter;
        ++counter;
        if (counter == 97) counter=0;
        if (writev(fd, &iovec1, 1) == -1)
            FATAL("write failed");
        //if (counter == 0)
         //   printf("write 97\n");
    }
    return 0;
}