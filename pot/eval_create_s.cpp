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
#define TST_NUM 10000000
int main(int argc, char* argv[])
{
    TimingInit();
    InitRdtsc();
    int core_num=atoi(argv[1]);
    pin_thread(core_num);
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    int property=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &property, sizeof(int));
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8080);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("failed to bind %s", strerror(errno));
    if (listen(fd, 100000) == -1)
        FATAL("listen failed");
    printf("listen succeed\n");
    while (1)
    {
        int connect_fd = accept4(fd, NULL, NULL, 0);
        if (connect_fd == -1)
            FATAL("Failed to connect to client");
        //close(connect_fd);
    }

    return 0;


}
