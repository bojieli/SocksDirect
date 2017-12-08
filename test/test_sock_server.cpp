//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include "../lib/lib_internal.h"
#include "../lib/socket_lib.h"

int main()
{
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
    uint8_t buffer[1024];
    unsigned int counter = 0;
    //int connect_fd=accept4(fd, NULL, 0, 0);
    while (1)
    {
        int connect_fd = accept4(fd, NULL, NULL, SOCK_NONBLOCK);
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
                    //printf("empty\n");
                    continue;
                } else
                    FATAL("Rd error!");
            }
            //printf("data received\n");
            if (len != 16)
                FATAL("length error");
            if (*(int *)buffer != counter)
            {
                auto thread_sock_data = GET_THREAD_SOCK_DATA();
                FATAL("data error should: %p, recvd: %p", counter, *(int *)buffer);
            }
            ++counter;
            if (counter % 1000000 == 0)
                printf("Read %u\n", counter);
        }
    }
}
