//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/lib.h"

char buffer[1024] = "stats\r\n";
char readbuf[65536];

int main()
{
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) FATAL("Failed to create fd");
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(11211);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        FATAL("Failed to connect");
    printf("connect succeed\n");
    struct iovec iovec1;
    iovec1.iov_len = strlen(buffer);
    iovec1.iov_base = (void *) &buffer;

    TimingInit();
    InitRdtsc();
    int first_time = 1;
    while (1) {
        struct timespec s_time, e_time;
        GetRdtscTime(&s_time);

        if (writev(fd, &iovec1, 1) == -1)
            FATAL("write failed");

        int readlen = read(fd, readbuf, sizeof(readbuf));
        if (readlen == -1) {
            FATAL("read failed");
            break;
        }
        else if (readlen == 0) { // eof
            break;
        }
        else {
            if (first_time) {
                first_time = 0;
                write(1, readbuf, readlen);
            }
            else {
                GetRdtscTime(&e_time);
                printf("lat = %lf us\n", (double)((e_time.tv_sec - s_time.tv_sec) * (double)1e6 + (e_time.tv_nsec - s_time.tv_nsec) / (double)1e3));
            }
        }

        sleep(1);
    }
    return 0;
}
