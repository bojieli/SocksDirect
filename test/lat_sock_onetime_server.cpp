//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include "../lib/lib_internal.h"
#include "../lib/socket_lib.h"
#include <time.h>

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
    int init_fd = accept4(fd, NULL, NULL, 0);
    uint8_t buffer[1024];
    recvfrom(init_fd, buffer, 1024, 0, NULL, NULL);
    int counter=0;
    struct timespec tv_begin;
    clock_gettime(CLOCK_REALTIME, &tv_begin);
    while (1)
    {
        int sock_fd = accept4(fd, NULL, NULL, 0);
        if (sock_fd < 0)
            FATAL("failed to accept");
        ++counter;
        recvfrom(sock_fd, buffer, 1024, 0, NULL, NULL);
        if (counter % 100000 == 0) {
            struct timespec tv_end;
            clock_gettime(CLOCK_REALTIME, &tv_end);
            unsigned long diff = (tv_end.tv_sec - tv_begin.tv_sec) * 1e9 + (tv_end.tv_nsec - tv_begin.tv_nsec);
            printf ("counter=%d, tput=%lf /s\n", counter, (double)counter * 1e9 / diff);
        }
    }
}
