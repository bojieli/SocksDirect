//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/pot_socket_lib.h"
uint8_t buffer[1048576]  __attribute__((aligned(PAGE_SIZE)));
#define T1RND 10000000

int main(int argc, char* argv[])
{

    pin_thread(0);

    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);
    if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect, %d", errno);
    printf("connect succeed\n");

    pot_init_write();
    TimingInit();
    InitRdtsc();
    for (int i=0;i<3*T1RND;++i)
    {
        pot_write_nbyte(fd, 8);
    }
    return 0;
}
