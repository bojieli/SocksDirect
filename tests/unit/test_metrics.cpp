// Unit tests for socksdirect::MetricsRegistry.

#include "socksdirect/metrics.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace {

TEST(Metrics, CounterBasic) {
    socksdirect::MetricsRegistry r;
    auto& c = r.counter("requests_total", "total requests");
    c.inc();
    c.inc(4);
    EXPECT_EQ(5.0, c.value());
    // Inc with negative is a no-op (counters are monotonic).
    c.inc(-1);
    EXPECT_EQ(5.0, c.value());
}

TEST(Metrics, GaugeBasic) {
    socksdirect::MetricsRegistry r;
    auto& g = r.gauge("queue_depth", "queue depth");
    g.set(3);
    EXPECT_EQ(3.0, g.value());
    g.inc();
    g.inc(2);
    EXPECT_EQ(6.0, g.value());
    g.dec(4);
    EXPECT_EQ(2.0, g.value());
}

TEST(Metrics, RenderCounterAndGauge) {
    socksdirect::MetricsRegistry r;
    r.counter("a_total", "alpha").inc(3);
    r.gauge("b", "beta").set(7);
    auto out = r.render();
    EXPECT_NE(std::string::npos, out.find("# HELP a_total alpha")) << out;
    EXPECT_NE(std::string::npos, out.find("# TYPE a_total counter")) << out;
    EXPECT_NE(std::string::npos, out.find("a_total 3")) << out;
    EXPECT_NE(std::string::npos, out.find("# HELP b beta")) << out;
    EXPECT_NE(std::string::npos, out.find("# TYPE b gauge")) << out;
    EXPECT_NE(std::string::npos, out.find("b 7")) << out;
}

TEST(Metrics, RenderLabels) {
    socksdirect::MetricsRegistry r;
    r.counter("rpc_total", "rpc", {{"transport", "shm"}}).inc(2);
    r.counter("rpc_total", "rpc", {{"transport", "rdma"}}).inc(5);
    auto out = r.render();
    EXPECT_NE(std::string::npos, out.find("rpc_total{transport=\"shm\"} 2")) << out;
    EXPECT_NE(std::string::npos, out.find("rpc_total{transport=\"rdma\"} 5")) << out;
}

TEST(Metrics, LabelEscaping) {
    socksdirect::MetricsRegistry r;
    r.counter("evil", "trying to break out", {{"k", "a\"b\\c\nd"}}).inc();
    auto out = r.render();
    EXPECT_NE(std::string::npos, out.find("k=\"a\\\"b\\\\c\\nd\"")) << out;
}

TEST(Metrics, Histogram) {
    socksdirect::MetricsRegistry r;
    auto& h = r.histogram("latency_ns", {100, 1000, 10000}, "wall");
    h.observe(50);     // bucket le=100
    h.observe(50);     // bucket le=100
    h.observe(500);    // bucket le=1000
    h.observe(20000);  // +Inf bucket
    auto snap = h.snapshot();
    EXPECT_EQ(2u, snap.cumulative[0]);  // <= 100
    EXPECT_EQ(3u, snap.cumulative[1]);  // <= 1000
    EXPECT_EQ(3u, snap.cumulative[2]);  // <= 10000
    EXPECT_EQ(4u, snap.cumulative[3]);  // <= +Inf
    EXPECT_EQ(4u, snap.count);
    EXPECT_DOUBLE_EQ(20600.0, snap.sum);

    auto out = r.render();
    EXPECT_NE(std::string::npos, out.find("# TYPE latency_ns histogram")) << out;
    EXPECT_NE(std::string::npos, out.find("latency_ns_bucket{le=\"100\"} 2")) << out;
    EXPECT_NE(std::string::npos, out.find("latency_ns_bucket{le=\"+Inf\"} 4")) << out;
    EXPECT_NE(std::string::npos, out.find("latency_ns_count 4")) << out;
}

TEST(Metrics, HistogramOnBucketBoundaryGoesIntoLowerBucket) {
    // Exact boundary belongs to the bucket whose le=value (per Prometheus).
    socksdirect::MetricsRegistry r;
    auto& h = r.histogram("x", {1.0, 2.0});
    h.observe(1.0);
    h.observe(2.0);
    auto snap = h.snapshot();
    EXPECT_EQ(1u, snap.cumulative[0]);  // 1.0 <= 1.0
    EXPECT_EQ(2u, snap.cumulative[1]);  // 2.0 <= 2.0
    EXPECT_EQ(2u, snap.cumulative[2]);  // +Inf
}

TEST(Metrics, ConcurrentInc) {
    socksdirect::MetricsRegistry r;
    auto& c = r.counter("hits");
    constexpr int kThreads = 8;
    constexpr int kPer = 50000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]() {
            for (int i = 0; i < kPer; ++i) c.inc();
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_DOUBLE_EQ(static_cast<double>(kThreads * kPer), c.value());
}

TEST(Metrics, MetricNamesEnumerated) {
    socksdirect::MetricsRegistry r;
    r.counter("z");
    r.counter("a");
    r.gauge("g");
    r.histogram("h", {1, 2, 3});
    auto names = r.metric_names();
    ASSERT_EQ(4u, names.size());
    EXPECT_EQ("a", names[0]);
    EXPECT_EQ("g", names[1]);
    EXPECT_EQ("h", names[2]);
    EXPECT_EQ("z", names[3]);
}

TEST(Metrics, RenderIsAlphabeticalWithinType) {
    socksdirect::MetricsRegistry r;
    r.counter("zebra").inc();
    r.counter("apple").inc();
    auto out = r.render();
    auto a = out.find("apple");
    auto z = out.find("zebra");
    ASSERT_NE(std::string::npos, a);
    ASSERT_NE(std::string::npos, z);
    EXPECT_LT(a, z);
}

}  // namespace
