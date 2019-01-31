//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../common/helper.h"
#include "../lib/lib.h"



#define MAX_MSGSIZE (1024*1024)
#define TST_NUM 10000
#define WARMUP_NUM 10000

#define NUM_BUFFERS 1024
uint8_t real_buffer[NUM_BUFFERS * MAX_MSGSIZE];
int main(int argc, char * argv[])
{
    if (argc < 3)
    {
        printf("Usage: <msgsize> <port>");
        return -1;
    }

    pin_thread(2);
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    int property=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &property, sizeof(int));

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(atoi(argv[2]));
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind %s", strerror(errno));
    if (listen(fd, 10) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");
    int msgsize=atoi(argv[1]);
    if (msgsize== 0)
    {
        printf("Invalid msg size");
        return -1;
    }



    int connect_fd = -1;
    while (connect_fd == -1) {
        connect_fd = accept(fd, NULL, NULL);
        if (connect_fd == -1 && (errno != EAGAIN && errno != EWOULDBLOCK))
            FATAL("Failed to connect to client, errno %d", errno);
    }
    int tmp = 1;
    setsockopt( connect_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
    tmp = MAX_MSGSIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &tmp, sizeof(tmp));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp));

    InitRdtsc();

    uint8_t *buffer = real_buffer;
    uint8_t *real_buffer_end = real_buffer + sizeof(real_buffer);

    for (int i=0;i<WARMUP_NUM+TST_NUM;++i)
    {
        int len = 0;
        while (len < msgsize)
            len += recvfrom(connect_fd, (void *) buffer+len, msgsize-len, 0, NULL, NULL);


        len = 0;
        while (len < msgsize)
        {
            len += write(connect_fd, (void *) buffer+len, msgsize-len);
        }

        buffer += msgsize;
        if (buffer >= real_buffer_end)
            buffer = real_buffer;
    }
}
