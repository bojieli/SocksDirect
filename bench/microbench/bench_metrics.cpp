// bench_metrics — measure MetricsRegistry counter inc throughput and
// rendering throughput. The metrics endpoint is on the /metrics scrape
// path; we want to know the cost of rendering a populated registry.

#include "socksdirect/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
        "{\"bench\":\"metrics\",\"submode\":\"%s\",\"mode\":\"throughput\","
        "\"msg_count\":%llu,\"elapsed_ns\":%llu,\"throughput_mps\":%.2f,"
        "\"p50_ns\":null,\"p99_ns\":null,\"p999_ns\":null}\n",
        sub,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(ns),
        mps);
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "throughput";
    long long count = 5000000;
    int threads = 1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--mode=", 0) == 0) mode = a.substr(7);
        else if (a.rfind("--count=", 0) == 0) count = std::atoll(a.c_str() + 8);
        else if (a.rfind("--threads=", 0) == 0) threads = std::atoi(a.c_str() + 10);
    }

    socksdirect::MetricsRegistry r;
    auto& c = r.counter("hits", "h");

    if (mode == "throughput") {
        // Single-threaded inc.
        uint64_t s = now_ns();
        for (long long i = 0; i < count; ++i) c.inc();
        uint64_t e = now_ns();
        print_json_throughput("counter_inc_st", count, e - s);

        if (threads > 1) {
            std::atomic<bool> go{false};
            std::vector<std::thread> ts;
            for (int t = 0; t < threads; ++t) {
                ts.emplace_back([&, t]() {
                    while (!go.load()) std::this_thread::yield();
                    long long per = count / threads;
                    for (long long i = 0; i < per; ++i) c.inc();
                });
            }
            uint64_t s2 = now_ns();
            go.store(true);
            for (auto& th : ts) th.join();
            uint64_t e2 = now_ns();
            print_json_throughput("counter_inc_mt", count, e2 - s2);
        }

        // Render throughput. Populate ~50 metrics and render N times.
        for (int i = 0; i < 50; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "metric_%d", i);
            r.counter(name, "h").inc(static_cast<double>(i));
        }
        const long long renders = std::min<long long>(count / 100, 100000);
        uint64_t bs = now_ns();
        std::size_t total_bytes = 0;
        for (long long i = 0; i < renders; ++i) {
            auto out = r.render();
            total_bytes += out.size();
        }
        uint64_t be = now_ns();
        print_json_throughput("render", renders, be - bs);
        return 0;
    }

    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
