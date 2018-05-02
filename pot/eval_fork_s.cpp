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

    int fds[2];
    pipe(fds);

    struct timespec s_time;
    GetRdtscTime(&s_time);
    int counter(0);
    for (int i=0;i<T1RND;++i)
    {
        pot_read_nbyte(connect_fd, buffer, 8);
        counter++;
        if ((counter & 0xFF) == 0)
        {
            struct timespec c_time;
            GetRdtscTime(&c_time);
            double duration;
            duration = (c_time.tv_sec - s_time.tv_sec) * 1e3 + (c_time.tv_nsec - s_time.tv_nsec) / 1e6;
            if (duration >= 1)
            {
                s_time = c_time;
                printf("%.0lf\n", (double)0xFF/duration);
            }
        }
    }
    if (fork() != 0)
    {
        //It is the parent
        for (int i=0;i<T1RND;++i)
        {
            pot_read_nbyte(connect_fd, buffer, 8);
            counter++;
            if ((counter & 0xFF) == 0)
            {
                struct timespec c_time;
                GetRdtscTime(&c_time);
                double duration;
                duration = (c_time.tv_sec - s_time.tv_sec) * 1e3 + (c_time.tv_nsec - s_time.tv_nsec) / 1e6;
                if (duration >= 1)
                {
                    s_time = c_time;
                    printf("%.0lf\n", (double)0xFF/duration);
                }
            }
        }
        write(fds[1], "a", 1);
    } else
    {
        read(fds[0],buffer,1);
        for (int i=0;i<T1RND;++i)
        {
            pot_read_nbyte(connect_fd, buffer, 8);
            counter++;
            if ((counter & 0xFF) == 0)
            {
                struct timespec c_time;
                GetRdtscTime(&c_time);
                double duration;
                duration = (c_time.tv_sec - s_time.tv_sec) * 1e3 + (c_time.tv_nsec - s_time.tv_nsec) / 1e6;
                if (duration >= 1)
                {
                    s_time = c_time;
                    printf("%.0lf\n", (double)0xFF/duration);
                }
            }
        }
    }

    return 0;

}
