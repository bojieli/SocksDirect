// Unit tests for ShmHandshakeRegistry.

#include "socksdirect/shm_handshake.hpp"

#include <gtest/gtest.h>
#include <set>
#include <thread>

namespace {

using socksdirect::ShmHandshakeRegistry;
using Role = ShmHandshakeRegistry::Role;

TEST(ShmHandshake, ConnIdIsCanonical) {
    auto a = ShmHandshakeRegistry::make("10.0.0.1:5000", "10.0.0.2:5001");
    auto b = ShmHandshakeRegistry::make("10.0.0.2:5001", "10.0.0.1:5000");
    EXPECT_EQ(a.a, b.a);
    EXPECT_EQ(a.b, b.b);
}

TEST(ShmHandshake, FirstCallerIsCreator) {
    ShmHandshakeRegistry r;
    auto id = ShmHandshakeRegistry::make("127.0.0.1:1", "127.0.0.1:2");
    auto res = r.register_endpoint(id, 100);
    EXPECT_EQ(Role::kCreator, res.role);
    EXPECT_EQ(100, res.pid_a);
    EXPECT_EQ(0,   res.pid_b);
    EXPECT_NE(0u,  res.key);
    EXPECT_EQ(1u,  r.live_pairs());
}

TEST(ShmHandshake, SecondCallerIsJoinerAndGetsSameKey) {
    ShmHandshakeRegistry r;
    auto id = ShmHandshakeRegistry::make("127.0.0.1:1", "127.0.0.1:2");
    auto res1 = r.register_endpoint(id, 100);
    auto res2 = r.register_endpoint(id, 200);
    EXPECT_EQ(Role::kJoiner, res2.role);
    EXPECT_EQ(res1.key, res2.key);
    EXPECT_EQ(100, res2.pid_a);
    EXPECT_EQ(200, res2.pid_b);
}

TEST(ShmHandshake, KeyIsNonZero) {
    ShmHandshakeRegistry r;
    r.seed_for_test(0);  // deterministic; first draw might be 0
    for (int i = 0; i < 16; ++i) {
        auto id = ShmHandshakeRegistry::make(
            "10.0.0.1:" + std::to_string(i), "10.0.0.2:0");
        auto res = r.register_endpoint(id, 1);
        EXPECT_NE(0u, res.key);
    }
}

TEST(ShmHandshake, UnregisterRemovesEntry) {
    ShmHandshakeRegistry r;
    auto id = ShmHandshakeRegistry::make("127.0.0.1:1", "127.0.0.1:2");
    r.register_endpoint(id, 100);
    EXPECT_TRUE(r.unregister_endpoint(id));
    EXPECT_EQ(0u, r.live_pairs());
    EXPECT_FALSE(r.unregister_endpoint(id));  // idempotent
}

TEST(ShmHandshake, ReapPidDropsAllOwnedPairs) {
    ShmHandshakeRegistry r;
    for (int i = 0; i < 5; ++i) {
        auto id = ShmHandshakeRegistry::make(
            "127.0.0.1:" + std::to_string(i), "127.0.0.1:1000");
        r.register_endpoint(id, 100);
    }
    // Other-pid pair shouldn't be touched.
    auto other = ShmHandshakeRegistry::make("127.0.0.1:9999", "127.0.0.1:1");
    r.register_endpoint(other, 200);
    EXPECT_EQ(6u, r.live_pairs());
    EXPECT_EQ(5u, r.reap_pid(100));
    EXPECT_EQ(1u, r.live_pairs());
}

TEST(ShmHandshake, KeysAreDistinctAcrossEndpoints) {
    ShmHandshakeRegistry r;
    std::set<uint64_t> keys;
    for (int i = 0; i < 50; ++i) {
        auto id = ShmHandshakeRegistry::make(
            "10.0.0.1:" + std::to_string(i), "10.0.0.2:0");
        keys.insert(r.register_endpoint(id, i + 1).key);
    }
    // Random64 collisions are vanishingly unlikely; expect 50 unique.
    EXPECT_EQ(50u, keys.size());
}

TEST(ShmHandshake, ConcurrentRegistersConvergeOnOneCreator) {
    ShmHandshakeRegistry r;
    auto id = ShmHandshakeRegistry::make("127.0.0.1:1", "127.0.0.1:2");
    constexpr int kThreads = 16;
    std::vector<ShmHandshakeRegistry::Result> results(kThreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t]() {
            results[t] = r.register_endpoint(id, t + 1000);
        });
    }
    for (auto& t : ts) t.join();
    int creators = 0;
    for (const auto& r0 : results) if (r0.role == Role::kCreator) ++creators;
    EXPECT_EQ(1, creators);
    // All threads see the same key.
    for (int i = 1; i < kThreads; ++i) {
        EXPECT_EQ(results[0].key, results[i].key);
    }
}

}  // namespace
