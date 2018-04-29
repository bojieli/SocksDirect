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

int main(int argc, char* argv[])
{
    if (argc < 2) FATAL("Lack of parameter: <output file name> <size of the message>");
    int warmup_num=10000000;
    int test_num=10000;
    int inner_test_num = 1;
    int test_size=atoi(argv[1]);

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
    uint8_t buffer[65536];

    pot_init_write();

    int connect_fd = accept4(fd, NULL, NULL, 0);
    if (connect_fd == -1)
        FATAL("Failed to connect to client");
    printf("Connected\n");

    for (int i=0;i<warmup_num + test_num;++i)
    {
        //pong
        for (int j=0;j< inner_test_num; ++j)
        {
            pot_read_nbyte(connect_fd, buffer, test_size);
            pot_write_nbyte(connect_fd, test_size);
        }
    }

}