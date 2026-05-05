// bench_queue_v3 — single-producer / single-consumer microbenchmark
// against locklessqueue_t_v3.
//
// What it measures:
//   - Throughput in messages per second.
//   - Median, p99, p99.9 ping-pong latency in nanoseconds (when run with
//     --mode=latency).
//
// Why standalone (no LD_PRELOAD): this is the input to the CI
// perf-regression gate. It must run on any Linux box, no kernel module,
// no RDMA NIC. It exercises the queue in isolation so a regression here
// localizes the culprit to the queue, not to the socket layer above.
//
// Output format: a single JSON object on stdout, suitable for
// machine-parsing by tools/perf_regression.py:
//
//   {
//     "bench": "queue_v3",
//     "mode": "throughput",
//     "msg_count": 10000000,
//     "elapsed_ns": 432156890,
//     "throughput_mps": 23123456.7,
//     "p50_ns": null,
//     "p99_ns": null,
//     "p999_ns": null
//   }
//
// Usage:
//   bench_queue_v3 --mode=throughput --count=10000000
//   bench_queue_v3 --mode=latency    --count=200000

#include "common/locklessq_v3.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kSize = 256;
using Q = locklessqueue_t_v3<int, kSize>;

struct alignas(64) Buffers {
    alignas(16) Q::element_t buf1[kSize];
    alignas(16) Q::element_t buf2[kSize];
    alignas(64) bool ret1 = false;
    alignas(64) bool ret2 = false;
};

uint64_t now_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void try_pin_thread(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Throughput: producer pushes `count` messages as fast as possible;
// consumer drains them. We measure wall-clock time from first push to
// last drain.
void run_throughput(int count) {
    Buffers b;
    Q producer, consumer;
    Q::mem_ptr_t p1{b.buf1, &b.ret1};
    Q::mem_ptr_t p2{b.buf2, &b.ret2};
    producer.init_ptr(p1, p2, false);
    consumer.init_ptr(p1, p2, true);
    producer.init_mem();

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> first_push_ns{0};

    auto producer_fn = [&] {
        try_pin_thread(2);
        first_push_ns.store(now_ns(), std::memory_order_release);
        for (int i = 0; i < count; ++i) {
            while (!producer.push_nb(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    };
    uint64_t last_drain_ns = 0;
    auto consumer_fn = [&] {
        try_pin_thread(4);
        int drained = 0;
        while (drained < count) {
            auto it = consumer.begin();
            if (!it.valid() || !it->isvalid) {
                std::this_thread::yield();
                continue;
            }
            it.del();
            ++drained;
        }
        last_drain_ns = now_ns();
    };

    std::thread tp(producer_fn);
    std::thread tc(consumer_fn);
    tp.join();
    tc.join();
    (void)producer_done;

    uint64_t elapsed = last_drain_ns - first_push_ns.load();
    double mps = static_cast<double>(count) * 1e9 / static_cast<double>(elapsed);

    std::printf(
        "{\"bench\":\"queue_v3\",\"mode\":\"throughput\","
        "\"msg_count\":%d,\"elapsed_ns\":%lu,"
        "\"throughput_mps\":%.3f,"
        "\"p50_ns\":null,\"p99_ns\":null,\"p999_ns\":null}\n",
        count, static_cast<unsigned long>(elapsed), mps);
}

// Latency: producer pushes one int with a sequence number; consumer pops
// it and (via a return queue) sends an ack with the same sequence number;
// producer measures the round trip. Repeated `count` times.
//
// We emulate the return-path with a second pair of queues since v3 is
// SPSC. This is a synthetic upper bound on what the queue can do — real
// socket ping-pong adds further overhead in socket_lib's pre/post hooks.
void run_latency(int count) {
    Buffers fwd, rev;
    Q fwd_p, fwd_c, rev_p, rev_c;
    fwd_p.init_ptr({fwd.buf1, &fwd.ret1}, {fwd.buf2, &fwd.ret2}, false);
    fwd_c.init_ptr({fwd.buf1, &fwd.ret1}, {fwd.buf2, &fwd.ret2}, true);
    rev_p.init_ptr({rev.buf1, &rev.ret1}, {rev.buf2, &rev.ret2}, false);
    rev_c.init_ptr({rev.buf1, &rev.ret1}, {rev.buf2, &rev.ret2}, true);
    fwd_p.init_mem();
    rev_p.init_mem();

    std::vector<uint64_t> samples(count);

    std::atomic<bool> server_ready{false};
    auto server = [&] {
        try_pin_thread(4);
        server_ready.store(true, std::memory_order_release);
        for (int i = 0; i < count; ++i) {
            int got;
            while (true) {
                auto it = fwd_c.begin();
                if (it.valid() && it->isvalid) {
                    got = it->data;
                    it.del();
                    break;
                }
                std::this_thread::yield();
            }
            while (!rev_p.push_nb(got)) {
                std::this_thread::yield();
            }
        }
    };

    std::thread ts(server);
    while (!server_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    try_pin_thread(2);

    for (int i = 0; i < count; ++i) {
        uint64_t t0 = now_ns();
        while (!fwd_p.push_nb(i)) std::this_thread::yield();
        while (true) {
            auto it = rev_c.begin();
            if (it.valid() && it->isvalid) {
                it.del();
                break;
            }
            std::this_thread::yield();
        }
        samples[i] = now_ns() - t0;
    }
    ts.join();

    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p * (samples.size() - 1));
        return samples[idx];
    };
    uint64_t p50 = pct(0.50), p99 = pct(0.99), p999 = pct(0.999);

    std::printf(
        "{\"bench\":\"queue_v3\",\"mode\":\"latency\","
        "\"msg_count\":%d,\"elapsed_ns\":null,"
        "\"throughput_mps\":null,"
        "\"p50_ns\":%lu,\"p99_ns\":%lu,\"p999_ns\":%lu}\n",
        count,
        static_cast<unsigned long>(p50),
        static_cast<unsigned long>(p99),
        static_cast<unsigned long>(p999));
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "throughput";
    int count = 1000000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--mode=", 0) == 0)       mode = a.substr(7);
        else if (a.rfind("--count=", 0) == 0) count = std::atoi(a.c_str() + 8);
        else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "usage: %s [--mode=throughput|latency] [--count=N]\n",
                argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    if (mode == "throughput") run_throughput(count);
    else if (mode == "latency") run_latency(count);
    else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }
    return 0;
}
