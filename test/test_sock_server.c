//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"

int main()
{
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind %s", strerror(errno));
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");
    uint8_t buffer[1024];
    uint8_t counter = 0;
    //int connect_fd=accept4(fd, NULL, 0, 0);
    while (1)
    {
        int connect_fd = accept4(fd, NULL, 0, SOCK_NONBLOCK);
        if (connect_fd == -1)
            FATAL("Failed to connect to client");
        while (1)
        {
            int len = recvfrom(connect_fd, (void *) buffer, 1024, 0, NULL, NULL);
            //printf("ret from recvfrom \n");
            if (len == -1)
            {
                if (errno == (EWOULDBLOCK | EAGAIN))
                {
                    continue;
                } else
                    FATAL("Rd error!");
            }
           // printf("data received\n");
            if (len != 1024)
                FATAL("length error");
            if (buffer[0] != counter)
                FATAL("data error should: %hhu, recvd: %hhu", counter, buffer[0]);
            ++counter;
            if (counter == 0)
                printf("Read 256\n");
        }
    }
}