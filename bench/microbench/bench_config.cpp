// bench_config — measure Config::get_string lookup throughput.
//
// Library code reads config on hot paths during connection setup
// (e.g. fd watermarks, log level). The current map-based lookup is
// O(log n); we want to know the absolute cost.

#include "socksdirect/config.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

uint64_t now_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void print_json(const char* sub, uint64_t count, uint64_t ns) {
    double mps = count * 1e9 / static_cast<double>(ns ? ns : 1);
    std::printf(
        "{\"bench\":\"config\",\"submode\":\"%s\",\"mode\":\"throughput\","
        "\"msg_count\":%llu,\"elapsed_ns\":%llu,\"throughput_mps\":%.2f,"
        "\"p50_ns\":null,\"p99_ns\":null,\"p999_ns\":null}\n",
        sub,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(ns),
        mps);
}

}  // namespace

int main(int argc, char** argv) {
    long long count = 5000000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--count=", 0) == 0) count = std::atoll(a.c_str() + 8);
    }

    char tmpl[] = "/tmp/socksdirect-bench-conf.XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { std::fprintf(stderr, "mkstemp failed\n"); return 1; }
    ::close(fd);
    {
        std::ofstream o(tmpl);
        o << "[monitor]\nlog_level=info\nsocket_path=/run/x.sock\n";
        o << "[rdma]\ndevice=mlx5_0\nqp_depth=128\n";
        o << "[fd]\nvirtual_fd_base=1073741824\n";
    }
    auto c = socksdirect::Config::load(tmpl);

    {
        uint64_t s = now_ns();
        volatile int sink = 0;
        for (long long i = 0; i < count; ++i) {
            std::string v = c.get_string("rdma", "device", "auto");
            sink += static_cast<int>(v.size());
        }
        uint64_t e = now_ns();
        (void)sink;
        print_json("get_string_hit", count, e - s);
    }
    {
        uint64_t s = now_ns();
        volatile int sink = 0;
        for (long long i = 0; i < count; ++i) {
            std::string v = c.get_string("missing", "key", "default");
            sink += static_cast<int>(v.size());
        }
        uint64_t e = now_ns();
        (void)sink;
        print_json("get_string_miss", count, e - s);
    }
    {
        uint64_t s = now_ns();
        volatile int sink = 0;
        for (long long i = 0; i < count; ++i) {
            sink += c.get_int("rdma", "qp_depth", 0);
        }
        uint64_t e = now_ns();
        (void)sink;
        print_json("get_int", count, e - s);
    }

    std::remove(tmpl);
    return 0;
}
