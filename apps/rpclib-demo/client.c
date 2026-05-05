// SPDX-License-Identifier: Apache-2.0
//
// rpclib-demo client — N round-trips, prints msg/s + p50/p99 latency.
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MSG 64

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static int cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int read_full(int fd, void* buf, size_t n) {
    char* p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

int main(int argc, char** argv) {
    long iters = argc >= 2 ? atol(argv[1]) : 100000;
    int port  = argc >= 3 ? atoi(argv[2]) : 57777;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("connect"); return 2; }

    char buf[MSG];
    memset(buf, 0xa5, MSG);
    uint64_t* samples = calloc((size_t)iters, sizeof(uint64_t));
    if (!samples) return 3;

    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; ++i) {
        uint64_t s = now_ns();
        if (send(fd, buf, MSG, 0) != MSG) { perror("send"); return 4; }
        if (read_full(fd, buf, MSG) < 0)  { perror("recv"); return 5; }
        samples[i] = now_ns() - s;
    }
    uint64_t t1 = now_ns();
    double mps = (double)iters * 1e9 / (double)(t1 - t0);
    qsort(samples, (size_t)iters, sizeof(uint64_t), cmp);
    printf("rpclib_client: %ld round-trips in %.3f ms, %.0f msg/s\n",
           iters, (t1 - t0) / 1e6, mps);
    printf("  p50 = %llu ns, p99 = %llu ns, p99.9 = %llu ns\n",
           (unsigned long long)samples[iters / 2],
           (unsigned long long)samples[(long)((double)iters * 0.99)],
           (unsigned long long)samples[(long)((double)iters * 0.999)]);
    close(fd);
    free(samples);
    return 0;
}
