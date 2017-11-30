//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"

int main()
{
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    if (bind(fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind");
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed");
}