// SPDX-License-Identifier: Apache-2.0
//
// linux_tcp_pingpong — Linux TCP loopback ping-pong baseline.
//
// Why standalone (no libsd, no LD_PRELOAD): this is the reference
// number that intra-host figures compare *against*. Every claim
// "SocksDirect is Nx faster than Linux at message size M" needs a
// matching Linux number measured on the same hardware. Producing
// it here makes the comparison reproducible without needing any
// of the SocksDirect runtime at all.
//
// Loopback (127.0.0.1) keeps everything on one host and exercises
// the kernel's TCP stack — same code path readers will benchmark
// SocksDirect against.
//
// Output: one JSON object per line, schema as in bench_queue_v3 so
// tools/perf_regression.py and the harness CSV converter can consume
// it without translation:
//
//   {
//     "bench": "linux_tcp_pingpong",
//     "submode": "msgsize=4096",
//     "mode": "latency",
//     "msg_count": 50000,
//     "elapsed_ns": 123456789,
//     "throughput_mps": null,
//     "p50_ns": 12000,
//     "p99_ns": 18000,
//     "p999_ns": 22000
//   }

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static int qsort_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int read_full(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    size_t left = n;
    while (left) {
        ssize_t r = recv(fd, p, left, 0);
        if (r <= 0) return -1;
        p += r; left -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    size_t left = n;
    while (left) {
        ssize_t r = send(fd, p, left, 0);
        if (r <= 0) return -1;
        p += r; left -= (size_t)r;
    }
    return 0;
}

static int run_pair(size_t msgsize, long iters,
                    uint64_t* out_total_ns, uint64_t* samples) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 2; }
    socklen_t l = sizeof(a);
    if (getsockname(srv, (struct sockaddr*)&a, &l) < 0) { perror("getsockname"); return 3; }
    if (listen(srv, 8) < 0) { perror("listen"); return 4; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 5; }
    if (pid == 0) {
        // Echo child.
        int s = accept(srv, NULL, NULL);
        if (s < 0) _exit(10);
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        char* buf = (char*)malloc(msgsize);
        if (!buf) _exit(11);
        for (long i = 0; i < iters; ++i) {
            if (read_full(s, buf, msgsize) < 0) _exit(12);
            if (write_full(s, buf, msgsize) < 0) _exit(13);
        }
        close(s); free(buf); _exit(0);
    }

    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) { perror("client socket"); return 20; }
    int nodelay = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    if (connect(c, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("connect"); return 21; }
    char* buf = (char*)malloc(msgsize);
    if (!buf) return 22;
    memset(buf, 0xa5, msgsize);

    uint64_t t0 = now_ns();
    for (long i = 0; i < iters; ++i) {
        uint64_t s = now_ns();
        if (write_full(c, buf, msgsize) < 0) return 23;
        if (read_full(c,  buf, msgsize) < 0) return 24;
        uint64_t e = now_ns();
        samples[i] = e - s;
    }
    uint64_t t1 = now_ns();
    *out_total_ns = t1 - t0;

    close(c); close(srv); free(buf);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "echo child status %d\n", st);
        return 25;
    }
    return 0;
}

int main(int argc, char** argv) {
    long iters = 50000;
    size_t msgsizes[] = {64, 1024, 4096, 16384, 65536, 0};
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--iters=", 8) == 0) iters = atol(argv[i] + 8);
    }

    for (int i = 0; msgsizes[i]; ++i) {
        size_t m = msgsizes[i];
        uint64_t total = 0;
        uint64_t* samples = (uint64_t*)calloc((size_t)iters, sizeof(uint64_t));
        if (!samples) return 99;
        int rc = run_pair(m, iters, &total, samples);
        if (rc) { free(samples); return rc; }

        // Throughput: round-trips per second * 2 (one msg each way).
        double mps = (double)iters * 2.0 * 1e9 / (double)total;
        printf("{\"bench\":\"linux_tcp_pingpong\",\"submode\":\"msgsize=%zu\",\"mode\":\"throughput\","
               "\"msg_count\":%ld,\"elapsed_ns\":%llu,\"throughput_mps\":%.2f,"
               "\"p50_ns\":null,\"p99_ns\":null,\"p999_ns\":null}\n",
               m, iters * 2, (unsigned long long)total, mps);

        // Latency: half-RTT.
        qsort(samples, (size_t)iters, sizeof(uint64_t), qsort_u64);
        uint64_t p50  = samples[iters / 2] / 2;
        uint64_t p99  = samples[(long)((double)iters * 0.99)]  / 2;
        uint64_t p999 = samples[(long)((double)iters * 0.999)] / 2;
        printf("{\"bench\":\"linux_tcp_pingpong\",\"submode\":\"msgsize=%zu\",\"mode\":\"latency\","
               "\"msg_count\":%ld,\"elapsed_ns\":null,\"throughput_mps\":null,"
               "\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu}\n",
               m, iters,
               (unsigned long long)p50,
               (unsigned long long)p99,
               (unsigned long long)p999);

        free(samples);
        fflush(stdout);
    }
    return 0;
}
