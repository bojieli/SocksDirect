// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::Metrics — header-only Prometheus-text metrics registry.
//
// What this is:
//   - A small registry of counters, gauges, and fixed-bucket histograms.
//   - A `render(out)` method emits Prometheus text format (the "0.0.4"
//     spec) suitable for serving via HTTP or piping over a Unix socket.
//   - All counters/gauges are atomic; histograms are atomic per bucket.
//   - Designed to live in the monitor and (via a small SHM mirror, future
//     work) inside the preload library.
//
// What this is NOT:
//   - A protobuf exposition encoder (text is fine, scrapers cope).
//   - A push-gateway client.
//   - Authoritative for high-cardinality labels: each (metric, label-tuple)
//     is a separate atomic and we don't garbage-collect them. The intended
//     label cardinality for socksdirect is O(10) — connection counts,
//     transport types, error reason codes — not unique IPs.

#ifndef SOCKSDIRECT_METRICS_HPP_
#define SOCKSDIRECT_METRICS_HPP_

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace socksdirect {

namespace metrics_detail {

// Format a double as Prometheus expects: NaN/Inf get textual sentinels,
// integers within range emit without trailing zeros.
inline std::string fmt_double(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Inf" : "+Inf";
    char buf[64];
    // Prometheus accepts up to %g precision; six significant digits is
    // plenty for counters/gauges and avoids scientific notation for
    // ordinary integers.
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

inline std::string fmt_labels(const std::vector<std::pair<std::string, std::string>>& labels) {
    if (labels.empty()) return {};
    std::string out = "{";
    bool first = true;
    for (const auto& kv : labels) {
        if (!first) out += ",";
        first = false;
        out += kv.first;
        out += "=\"";
        for (char c : kv.second) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                default:   out += c;
            }
        }
        out += '"';
    }
    out += '}';
    return out;
}

}  // namespace metrics_detail

class Counter {
public:
    void inc(double v = 1.0) {
        if (v < 0) return;  // counters are monotonic
        // float counters; fp atomics avoid std::atomic<double> portability woes.
        uint64_t old = bits_.load(std::memory_order_relaxed);
        for (;;) {
            double cur;
            std::memcpy(&cur, &old, sizeof(cur));
            double next = cur + v;
            uint64_t bits;
            std::memcpy(&bits, &next, sizeof(bits));
            if (bits_.compare_exchange_weak(old, bits, std::memory_order_relaxed))
                return;
        }
    }
    double value() const {
        uint64_t v = bits_.load(std::memory_order_relaxed);
        double d;
        std::memcpy(&d, &v, sizeof(d));
        return d;
    }
private:
    std::atomic<uint64_t> bits_{0};
};

class Gauge {
public:
    void set(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        bits_.store(bits, std::memory_order_relaxed);
    }
    void inc(double v = 1.0) { add(v); }
    void dec(double v = 1.0) { add(-v); }
    void add(double v) {
        uint64_t old = bits_.load(std::memory_order_relaxed);
        for (;;) {
            double cur;
            std::memcpy(&cur, &old, sizeof(cur));
            double next = cur + v;
            uint64_t bits;
            std::memcpy(&bits, &next, sizeof(bits));
            if (bits_.compare_exchange_weak(old, bits, std::memory_order_relaxed))
                return;
        }
    }
    double value() const {
        uint64_t v = bits_.load(std::memory_order_relaxed);
        double d;
        std::memcpy(&d, &v, sizeof(d));
        return d;
    }
private:
    std::atomic<uint64_t> bits_{0};
};

class Histogram {
public:
    // Buckets must be strictly increasing; +Inf is implicit (always present).
    explicit Histogram(std::vector<double> buckets)
        : buckets_(std::move(buckets)), counts_(buckets_.size() + 1) {
        // sanity: enforce sorted (debug build catches mistakes).
        for (std::size_t i = 1; i < buckets_.size(); ++i) {
            if (buckets_[i] <= buckets_[i - 1]) {
                std::fprintf(stderr, "socksdirect: histogram buckets not sorted\n");
                std::abort();
            }
        }
    }

    void observe(double v) {
        std::size_t idx = std::lower_bound(buckets_.begin(), buckets_.end(), v) - buckets_.begin();
        // lower_bound gives first bucket >= v; for "le" semantics we want the same.
        if (idx > buckets_.size()) idx = buckets_.size();
        counts_[idx].fetch_add(1, std::memory_order_relaxed);
        // Sum and total count (cumulative).
        sum_bits_.fetch_add(0);  // touch ordering
        // Update sum atomically.
        uint64_t old = sum_bits_.load(std::memory_order_relaxed);
        for (;;) {
            double cur;
            std::memcpy(&cur, &old, sizeof(cur));
            double next = cur + v;
            uint64_t bits;
            std::memcpy(&bits, &next, sizeof(bits));
            if (sum_bits_.compare_exchange_weak(old, bits, std::memory_order_relaxed)) break;
        }
        total_.fetch_add(1, std::memory_order_relaxed);
    }

    struct Snapshot {
        std::vector<double> buckets;
        // counts[i] = how many observations <= buckets[i]; counts.back() = +Inf.
        std::vector<uint64_t> cumulative;
        double sum;
        uint64_t count;
    };

    Snapshot snapshot() const {
        Snapshot s;
        s.buckets = buckets_;
        s.cumulative.resize(buckets_.size() + 1);
        uint64_t acc = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            acc += counts_[i].load(std::memory_order_relaxed);
            s.cumulative[i] = acc;
        }
        uint64_t v = sum_bits_.load(std::memory_order_relaxed);
        std::memcpy(&s.sum, &v, sizeof(s.sum));
        s.count = total_.load(std::memory_order_relaxed);
        return s;
    }

    const std::vector<double>& bucket_bounds() const { return buckets_; }

private:
    std::vector<double> buckets_;
    std::vector<std::atomic<uint64_t>> counts_;
    std::atomic<uint64_t> sum_bits_{0};
    std::atomic<uint64_t> total_{0};
};

class MetricsRegistry {
public:
    enum Type { kCounter, kGauge, kHistogram };

    struct CounterEntry {
        std::string help;
        // map keyed by stringified labels for stable iteration.
        std::map<std::string, std::shared_ptr<Counter>> by_labels;
    };
    struct GaugeEntry {
        std::string help;
        std::map<std::string, std::shared_ptr<Gauge>> by_labels;
    };
    struct HistogramEntry {
        std::string help;
        std::vector<double> buckets;
        std::map<std::string, std::shared_ptr<Histogram>> by_labels;
    };

    Counter& counter(const std::string& name, const std::string& help = "",
                     const std::vector<std::pair<std::string, std::string>>& labels = {}) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = counters_[name];
        if (!e.help.empty() && !help.empty() && e.help != help) {
            // Keep the first registered help text; mismatched help is a bug.
        }
        if (e.help.empty()) e.help = help;
        std::string key = metrics_detail::fmt_labels(labels);
        auto it = e.by_labels.find(key);
        if (it == e.by_labels.end()) {
            it = e.by_labels.emplace(key, std::make_shared<Counter>()).first;
        }
        return *it->second;
    }

    Gauge& gauge(const std::string& name, const std::string& help = "",
                 const std::vector<std::pair<std::string, std::string>>& labels = {}) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = gauges_[name];
        if (e.help.empty()) e.help = help;
        std::string key = metrics_detail::fmt_labels(labels);
        auto it = e.by_labels.find(key);
        if (it == e.by_labels.end()) {
            it = e.by_labels.emplace(key, std::make_shared<Gauge>()).first;
        }
        return *it->second;
    }

    Histogram& histogram(const std::string& name, std::vector<double> buckets,
                         const std::string& help = "",
                         const std::vector<std::pair<std::string, std::string>>& labels = {}) {
        std::lock_guard<std::mutex> g(mu_);
        auto& e = histograms_[name];
        if (e.help.empty()) e.help = help;
        if (e.buckets.empty()) e.buckets = buckets;
        std::string key = metrics_detail::fmt_labels(labels);
        auto it = e.by_labels.find(key);
        if (it == e.by_labels.end()) {
            it = e.by_labels.emplace(key,
                std::make_shared<Histogram>(buckets)).first;
        }
        return *it->second;
    }

    // Render in Prometheus text format. Stable ordering: counters, then
    // gauges, then histograms; alphabetical by metric name.
    std::string render() const {
        std::lock_guard<std::mutex> g(mu_);
        std::ostringstream out;
        for (const auto& kv : counters_) {
            const auto& name = kv.first;
            const auto& e = kv.second;
            out << "# HELP " << name << " " << e.help << "\n";
            out << "# TYPE " << name << " counter\n";
            for (const auto& lkv : e.by_labels) {
                out << name << lkv.first << " "
                    << metrics_detail::fmt_double(lkv.second->value()) << "\n";
            }
        }
        for (const auto& kv : gauges_) {
            const auto& name = kv.first;
            const auto& e = kv.second;
            out << "# HELP " << name << " " << e.help << "\n";
            out << "# TYPE " << name << " gauge\n";
            for (const auto& lkv : e.by_labels) {
                out << name << lkv.first << " "
                    << metrics_detail::fmt_double(lkv.second->value()) << "\n";
            }
        }
        for (const auto& kv : histograms_) {
            const auto& name = kv.first;
            const auto& e = kv.second;
            out << "# HELP " << name << " " << e.help << "\n";
            out << "# TYPE " << name << " histogram\n";
            for (const auto& lkv : e.by_labels) {
                auto snap = lkv.second->snapshot();
                std::string base = lkv.first;
                std::string label_inner = base.size() >= 2 ? base.substr(1, base.size() - 2) : "";
                for (std::size_t i = 0; i < snap.buckets.size(); ++i) {
                    std::string le = "le=\"" + metrics_detail::fmt_double(snap.buckets[i]) + "\"";
                    std::string lab = label_inner.empty() ? "{" + le + "}" : "{" + label_inner + "," + le + "}";
                    out << name << "_bucket" << lab << " " << snap.cumulative[i] << "\n";
                }
                std::string le_inf = label_inner.empty() ? "{le=\"+Inf\"}" : "{" + label_inner + ",le=\"+Inf\"}";
                out << name << "_bucket" << le_inf << " " << snap.cumulative.back() << "\n";
                out << name << "_sum"   << base << " " << metrics_detail::fmt_double(snap.sum)   << "\n";
                out << name << "_count" << base << " " << snap.count << "\n";
            }
        }
        return out.str();
    }

    // For tests: expose registered metric names without exposing values.
    std::vector<std::string> metric_names() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::string> n;
        for (const auto& kv : counters_)   n.push_back(kv.first);
        for (const auto& kv : gauges_)     n.push_back(kv.first);
        for (const auto& kv : histograms_) n.push_back(kv.first);
        std::sort(n.begin(), n.end());
        return n;
    }

    static MetricsRegistry& instance() {
        static MetricsRegistry r;
        return r;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, CounterEntry>   counters_;
    std::map<std::string, GaugeEntry>     gauges_;
    std::map<std::string, HistogramEntry> histograms_;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_METRICS_HPP_
