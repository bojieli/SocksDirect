// Unit tests for socksdirect::FdRemapTable.
//
// This is the testable replacement for the static-global fd remap table
// currently inlined in lib/socket_lib.cpp. The production code will move
// onto this header in Phase 3.

#include "socksdirect/fd_remap.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using socksdirect::FdRemapTable;
using socksdirect::FdType;

TEST(FdRemap, EmptyOnConstruction) {
    FdRemapTable t;
    EXPECT_EQ(0u, t.live_count());
    auto m = t.lookup(0);
    EXPECT_EQ(socksdirect::kUnknown, m.type);
    EXPECT_EQ(-1, m.real_fd);
}

TEST(FdRemap, AllocReturnsAscendingVfdsFirst) {
    FdRemapTable t;
    EXPECT_EQ(0, t.alloc(socksdirect::kSystem, 3));
    EXPECT_EQ(1, t.alloc(socksdirect::kSocket, 4));
    EXPECT_EQ(2, t.alloc(socksdirect::kEpoll, 5));
}

TEST(FdRemap, LookupReturnsTypeAndRealFd) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 42);
    auto m = t.lookup(v);
    EXPECT_EQ(socksdirect::kSocket, m.type);
    EXPECT_EQ(42, m.real_fd);
}

TEST(FdRemap, ReverseLookup) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 99);
    EXPECT_EQ(v, t.reverse_lookup(socksdirect::kSocket, 99));
    EXPECT_EQ(-1, t.reverse_lookup(socksdirect::kSocket, 100));
    EXPECT_EQ(-1, t.reverse_lookup(socksdirect::kEpoll, 99));
}

TEST(FdRemap, FreeMakesVfdLookupUnknown) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 42);
    t.free(v);
    auto m = t.lookup(v);
    EXPECT_EQ(socksdirect::kUnknown, m.type);
    EXPECT_EQ(-1, m.real_fd);
    EXPECT_EQ(-1, t.reverse_lookup(socksdirect::kSocket, 42));
}

TEST(FdRemap, FreeIsIdempotent) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 42);
    t.free(v);
    // Calling free a second time on the same vfd is allowed and a no-op.
    t.free(v);
    // The "double free" must not corrupt the free list (the next alloc
    // should still produce a sane vfd).
    int v2 = t.alloc(socksdirect::kSocket, 50);
    EXPECT_EQ(v, v2);  // reuses the freed slot
}

TEST(FdRemap, AllocReusesFreedVfds) {
    FdRemapTable t;
    int a = t.alloc(socksdirect::kSocket, 1);  // 0
    int b = t.alloc(socksdirect::kSocket, 2);  // 1
    int c = t.alloc(socksdirect::kSocket, 3);  // 2
    t.free(b);
    int d = t.alloc(socksdirect::kSocket, 4);
    EXPECT_EQ(b, d);
    (void)a; (void)c;
}

TEST(FdRemap, NegativeRealFdPassesThrough) {
    FdRemapTable t;
    EXPECT_EQ(-1, t.alloc(socksdirect::kSocket, -1));
    EXPECT_EQ(-13, t.alloc(socksdirect::kSocket, -13));
    EXPECT_EQ(0u, t.live_count());
}

TEST(FdRemap, NegativeVfdLookupReturnsUnknown) {
    FdRemapTable t;
    auto m = t.lookup(-1);
    EXPECT_EQ(socksdirect::kUnknown, m.type);
    EXPECT_EQ(-1, m.real_fd);
}

TEST(FdRemap, GrowsBeyondInitialCapacity) {
    FdRemapTable t;
    constexpr int kN = 5000;
    std::vector<int> vfds;
    vfds.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        int v = t.alloc(socksdirect::kSocket, i);
        EXPECT_EQ(i, v);
        vfds.push_back(v);
    }
    for (int i = 0; i < kN; ++i) {
        auto m = t.lookup(vfds[i]);
        EXPECT_EQ(socksdirect::kSocket, m.type);
        EXPECT_EQ(i, m.real_fd);
    }
    EXPECT_EQ(static_cast<size_t>(kN), t.live_count());
}

TEST(FdRemap, SetBindsExplicitVfd) {
    FdRemapTable t;
    t.set(100, socksdirect::kSocket, 42);
    auto m = t.lookup(100);
    EXPECT_EQ(socksdirect::kSocket, m.type);
    EXPECT_EQ(42, m.real_fd);
    EXPECT_EQ(100, t.reverse_lookup(socksdirect::kSocket, 42));
    // Subsequent alloc must skip the watermarked region.
    int v = t.alloc(socksdirect::kSocket, 99);
    EXPECT_GT(v, 100);
}

TEST(FdRemap, ConcurrentAllocsAreUnique) {
    // Stress: N threads each allocating M vfds; verify no duplicates.
    FdRemapTable t;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;
    std::vector<std::vector<int>> per_thread(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back([&, tid] {
            per_thread[tid].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                per_thread[tid].push_back(
                    t.alloc(socksdirect::kSocket, tid * kPerThread + i));
            }
        });
    }
    for (auto& th : threads) th.join();

    std::unordered_set<int> all;
    for (auto& v : per_thread) {
        for (int vfd : v) {
            EXPECT_TRUE(all.insert(vfd).second) << "duplicate vfd=" << vfd;
        }
    }
    EXPECT_EQ(kThreads * kPerThread, static_cast<int>(all.size()));
    EXPECT_EQ(static_cast<size_t>(kThreads * kPerThread), t.live_count());
}

TEST(FdRemap, ConcurrentAllocFreeChurn) {
    // Each thread alternates alloc/free; afterwards live_count must be the
    // residual count.
    FdRemapTable t;
    constexpr int kThreads = 4;
    std::atomic<int> residual{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back([&, tid] {
            std::vector<int> mine;
            for (int i = 0; i < 2000; ++i) {
                mine.push_back(t.alloc(socksdirect::kSocket, tid * 10000 + i));
                if ((i & 1) == 0 && !mine.empty()) {
                    t.free(mine.back());
                    mine.pop_back();
                }
            }
            residual.fetch_add(static_cast<int>(mine.size()));
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(static_cast<size_t>(residual.load()), t.live_count());
}

}  // namespace
