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
    const int FD_NUM=128;
    int fds[FD_NUM];
    int counter=0;
    uint8_t buffer[1024];
    while (1)
    {
        for (int i=0;i<FD_NUM;++i)
        {
            fds[i] = accept4(fd, NULL, NULL, 0);
            if (fds[i] < 0)
                FATAL("accept failed");
        }
        printf("1\n");
        for (int i=0;i<FD_NUM;++i)
        {
            int len = -1;
            // loop until received the data
            do {
                len = recvfrom(fds[i], (void *) buffer, 1024, 0, NULL, NULL);
                if (len == -1)
                {
                    if (errno == (EWOULDBLOCK | EAGAIN))
                    {
                        //printf("empty\n");
                        continue;
                    } else
                        FATAL("Rd error!");
                }
            } while (len == -1);

            if (len != 1024)
                FATAL("length error, received %d bytes, should be 1024 bytes", len);
            if (*(int *)buffer != i)
                FATAL("Data error should: %p, recvd: %p", i, *(int *)buffer);
            printf("recvd %d\n",MAX_FD_ID-fds[i]);
        }
        printf("2\n");
        for (int i=0;i<FD_NUM;++i)
        {
            printf("close %d\n",MAX_FD_ID-fds[i]);
            if (close(fds[i]) != 0)
                FATAL("close failed");
        }
        printf("3\n");
        ++counter;
        printf("Server: Completed %d connections\n", counter * FD_NUM);
    }
}
