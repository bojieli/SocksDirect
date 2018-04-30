//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../common/helper.h"
#include "../lib/lib.h"



#define MAX_MSGSIZE (1024*1024)


#define MAX_CONN_NUM 100000000
int fds[MAX_CONN_NUM];

uint8_t buffer[MAX_MSGSIZE];
int numofconn;


int main(int argc, char * argv[])
{
    if (argc < 2)
    {
        printf("Usage: <numofconn>");
        return -1;
    }

    pin_thread(2);
    TimingInit();
    InitRdtsc();
    numofconn=atoi(argv[1]);
    for (int i=0;i<8;++i) buffer[i] = rand() % 256;
    if (numofconn == 0)
    {
        FATAL("Illegal number of connection");
        return -1;
    }

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
    if (listen(fd, 100000) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");

    for (int i=0;i<numofconn;++i)
    {
        fds[i] = accept4(fd, NULL, NULL, 0);
        if (fds[i] == -1)
            FATAL("Failed to connect to client");
        int tmp = 1;
        //setsockopt(fds[i], IPPROTO_TCP, TCP_NODELAY, (void *)&tmp, sizeof(tmp));
    }


    while (1)
    {
        for (int i=0;i<numofconn;++i)
        {
            int len(0);
            while (len<8) {
                int one_len = (int)write(fds[i], buffer + len, 8 - len);
                if (one_len == -1) {
                    printf("End\n");
                    return 0;
                }
                len += one_len;
            }
        }
    }
    return 0;
}
