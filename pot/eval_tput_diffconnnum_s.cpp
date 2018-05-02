//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include "../common/helper.h"
#include "../lib/lib.h"
#include "../lib/lib_internal.h"
#include "../lib/socket_lib.h"
#include "../lib/pot_socket_lib.h"
#define T1RND 10000000
uint8_t buffer[1048576]  __attribute__((aligned(PAGE_SIZE)));

int main(int argc, char* argv[])
{
    pin_thread(2);
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

    pot_init_write();
    TimingInit();
    InitRdtsc();
    int connect_fd = accept4(fd, NULL, NULL, 0);
    if (connect_fd == -1)
        FATAL("Failed to connect to client");
    printf("Connected\n");

    struct timespec e_time, s_time;
    GetRdtscTime(&s_time);
   for (int i=0;i<3*T1RND;++i)
   {
       pot_read_nbyte(connect_fd, buffer, 8);
   }
    GetRdtscTime(&e_time);
   printf("%.0lf\n", (double)3*T1RND / ((e_time.tv_sec-s_time.tv_sec) + (e_time.tv_nsec - s_time.tv_nsec)/1e9) / 1e3);

}
