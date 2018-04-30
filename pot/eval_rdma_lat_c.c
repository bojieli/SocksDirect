//
// Created by ctyi on 11/30/17.
//

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/helper.h"
#include "../lib/pot_socket_lib.h"
uint8_t buffer[65536];
int main(int argc, char* argv[])
{
    const int warmup_num = 10000000;
    const int test_num=10000;
    double samples[test_num];

    if (argc < 3) FATAL("Lack of parameter: <output file name> <size of the message>");
    int inner_test_num = 1;
    int test_size=atoi(argv[2]);
    FILE* data_output_f = fopen(argv[1], "w");
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
    for (int i=0;i<warmup_num + test_num;++i)
    {
        //get time
        struct timespec s_time, e_time;
        //clock_gettime(CLOCK_REALTIME, &s_time);
        GetRdtscTime(&s_time);

        for (int j=0;j<inner_test_num; ++j)
        {
            //ping
            pot_write_nbyte(fd, test_size);
            //pong
            pot_read_nbyte(fd, buffer, test_size);
        }

        //get time
        //clock_gettime(CLOCK_REALTIME, &e_time);
        GetRdtscTime(&e_time);

        if (i >= warmup_num)
            samples[i - warmup_num]= (double)((e_time.tv_sec - s_time.tv_sec) * (double)1e9 + (e_time.tv_nsec - s_time.tv_nsec)) / inner_test_num;
    }

    for (int i=0;i<test_num;++i) fprintf(data_output_f, "%.0lf\n", samples[i]);
    fclose(data_output_f);
    return 0;
}
