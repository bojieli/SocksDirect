//
// Created by ctyi on 11/23/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
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
    int counter=0;
    int init_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(init_fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect");
    if (fork() == 0)
    {
        printf("Child\n");
    } else
    {
        printf("Parent\n");
        //try to take over
        sleep(1);
        read(init_fd, buffer, 1);
        assert(buffer[0] == 1);
        sleep(2);
        read(init_fd, buffer, 1);
        assert(buffer[0] == 2);
        printf("Fin\n");
        while(1);
    }
    close(init_fd);
    return 0;
}