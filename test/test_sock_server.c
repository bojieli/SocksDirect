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
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind");
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");
    uint8_t buffer[1024];
    int counter=0;
    while (1)
    {
        int connect_fd = accept4(fd, NULL, 0, 0);
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
                }
                else
                    FATAL("Rd error!");
            }
            printf("data received\n");
            if (len!=1024)
                FATAL("length error");
            //for (int i=0;i<1024;++i)
            //{
            //    if (buffer[i] != (uint8_t)(i%256))
            //        FATAL("data error");
            //}
            if (buffer[0] != counter)
                FATAL("data error");
            ++counter;
        }
    }
}