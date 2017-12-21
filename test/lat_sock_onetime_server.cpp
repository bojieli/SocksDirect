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
    int init_fd = accept4(fd, NULL, NULL, 0);
    uint8_t buffer[1024];
    recvfrom(init_fd, buffer, 1024, 0, NULL, NULL);
    int counter=0;
    while (1)
    {
        int sock_fd = accept4(fd, NULL, NULL, 0);
        if (sock_fd < 0)
            FATAL("failed to accept");
        ++counter;
        recvfrom(sock_fd, buffer, 1024, 0, NULL, NULL);
        if (counter % 1000 == 0)
            printf("1k\n");
        printf("recv 1\n");
    }
}
