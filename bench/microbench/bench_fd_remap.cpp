// bench_fd_remap — measure FdRemapTable alloc/lookup/free throughput.
//
// The fd remap table sits on the syscall hot path: every intercepted
// open/socket/accept calls alloc() and every read/write/close calls
// lookup(). The single big-mutex implementation in the prototype
// becomes a bottleneck under high QPS and we want a steady baseline.
//
// Output: one JSON object per line, schema as in bench_queue_v3.

#include "socksdirect/fd_remap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

uint64_t now_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void print_json_throughput(const char* sub, uint64_t count, uint64_t ns) {
    double mps = count * 1e9 / static_cast<double>(ns ? ns : 1);
    std::printf(
        "{\"bench\":\"fd_remap\",\"submode\":\"%s\",\"mode\":\"throughput\","
        "\"msg_count\":%llu,\"elapsed_ns\":%llu,\"throughput_mps\":%.2f,"
        "\"p50_ns\":null,\"p99_ns\":null,\"p999_ns\":null}\n",
        sub,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(ns),
        mps);
}

void print_json_latency(const char* sub, uint64_t count, uint64_t total_ns,
                        const std::vector<uint64_t>& sorted) {
    auto pick = [&](double q) {
        std::size_t idx = static_cast<std::size_t>(q * (sorted.size() - 1));
        return sorted[idx];
    };
    std::printf(
        "{\"bench\":\"fd_remap\",\"submode\":\"%s\",\"mode\":\"latency\","
        "\"msg_count\":%llu,\"elapsed_ns\":%llu,\"throughput_mps\":null,"
        "\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu}\n",
        sub,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(total_ns),
        static_cast<unsigned long long>(pick(0.50)),
        static_cast<unsigned long long>(pick(0.99)),
        static_cast<unsigned long long>(pick(0.999)));
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "throughput";
    long long count = 1000000;
    int threads = 1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--mode=", 0) == 0)        mode = a.substr(7);
        else if (a.rfind("--count=", 0) == 0)  count = std::atoll(a.c_str() + 8);
        else if (a.rfind("--threads=", 0) == 0) threads = std::atoi(a.c_str() + 10);
    }

    if (mode == "throughput") {
        // alloc + lookup + free, single thread.
        socksdirect::FdRemapTable t;
        std::vector<int> vfds;
        vfds.reserve(count);
        uint64_t t0 = now_ns();
        for (long long i = 0; i < count; ++i) {
            int v = t.alloc(socksdirect::kSocket, static_cast<int>(i));
            vfds.push_back(v);
        }
        uint64_t t1 = now_ns();
        for (int v : vfds) (void)t.lookup(v);
        uint64_t t2 = now_ns();
        for (int v : vfds) t.free(v);
        uint64_t t3 = now_ns();

        print_json_throughput("alloc",  count, t1 - t0);
        print_json_throughput("lookup", count, t2 - t1);
        print_json_throughput("free",   count, t3 - t2);

        // Concurrent lookup throughput (alloc once, then thrash lookups).
        if (threads > 1) {
            socksdirect::FdRemapTable t2tab;
            std::vector<int> vv;
            for (long long i = 0; i < count; ++i)
                vv.push_back(t2tab.alloc(socksdirect::kSocket, static_cast<int>(i)));
            std::atomic<bool> go{false};
            std::vector<std::thread> ts;
            uint64_t total = 0;
            std::atomic<uint64_t> hits{0};
            for (int n = 0; n < threads; ++n) {
                ts.emplace_back([&, n]() {
                    while (!go.load()) std::this_thread::yield();
                    long long per = count / threads;
                    long long start = n * per;
                    for (long long i = 0; i < per; ++i) {
                        (void)t2tab.lookup(vv[start + i]);
                        hits.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            uint64_t s = now_ns();
            go.store(true);
            for (auto& th : ts) th.join();
            uint64_t e = now_ns();
            total = hits.load();
            print_json_throughput("lookup_mt", total, e - s);
        }
        return 0;
    }

    if (mode == "latency") {
        // Per-op alloc-and-free latency under no contention.
        socksdirect::FdRemapTable t;
        std::vector<uint64_t> samples;
        samples.reserve(count);
        uint64_t total_ns = 0;
        for (long long i = 0; i < count; ++i) {
            uint64_t s = now_ns();
            int v = t.alloc(socksdirect::kSocket, 1);
            t.free(v);
            uint64_t e = now_ns();
            samples.push_back(e - s);
            total_ns += e - s;
        }
        std::sort(samples.begin(), samples.end());
        print_json_latency("alloc_free", count, total_ns, samples);
        return 0;
    }

    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
