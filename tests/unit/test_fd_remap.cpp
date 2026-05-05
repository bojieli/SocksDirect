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

// ---------------------------------------------------------------------------
// Refcount tests (added for Phase 3: dup / dup2 / dup3 implementation)
// ---------------------------------------------------------------------------

TEST(FdRemap, AllocStartsRefcountAt1) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 7);
    EXPECT_EQ(1, t.refcount(v));
}

TEST(FdRemap, FreeReturnsZeroOnLastRef) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 7);
    EXPECT_EQ(0, t.free(v));   // last ref -> 0; caller may close real_fd
    EXPECT_EQ(0, t.refcount(v));
    // Idempotent.
    EXPECT_EQ(0, t.free(v));
}

TEST(FdRemap, DupCreatesDistinctVfdSharingResource) {
    FdRemapTable t;
    int v1 = t.alloc(socksdirect::kSocket, 7);
    int v2 = t.dup(v1);
    EXPECT_NE(v1, v2);
    EXPECT_GE(v2, 0);
    EXPECT_EQ(7, t.lookup(v2).real_fd);
    EXPECT_EQ(socksdirect::kSocket, t.lookup(v2).type);
    EXPECT_EQ(2, t.refcount(v1));
    EXPECT_EQ(2, t.refcount(v2));
}

TEST(FdRemap, FreeDecrementsRefcount) {
    FdRemapTable t;
    int v1 = t.alloc(socksdirect::kSocket, 7);
    int v2 = t.dup(v1);
    int v3 = t.dup(v1);
    EXPECT_EQ(3, t.refcount(v1));
    EXPECT_EQ(2, t.free(v2));   // 2 left
    EXPECT_EQ(1, t.free(v3));   // 1 left
    EXPECT_EQ(0, t.free(v1));   // last
}

TEST(FdRemap, DupOfUnmappedFails) {
    FdRemapTable t;
    errno = 0;
    EXPECT_EQ(-1, t.dup(99));
    EXPECT_EQ(EBADF, errno);
}

TEST(FdRemap, DupOfNegativeFails) {
    FdRemapTable t;
    errno = 0;
    EXPECT_EQ(-1, t.dup(-1));
    EXPECT_EQ(EBADF, errno);
}

TEST(FdRemap, DupToMovesBindingAndDecrementsOldTarget) {
    FdRemapTable t;
    int src = t.alloc(socksdirect::kSocket, 7);
    int dst = t.alloc(socksdirect::kSocket, 8);
    int prev_refs = -1;
    auto prev = t.dup_to(src, dst, &prev_refs);
    EXPECT_EQ(socksdirect::kSocket, prev.type);
    EXPECT_EQ(8, prev.real_fd);
    EXPECT_EQ(0, prev_refs);  // old binding had refcount 1; now 0
    // dst now points at real_fd=7.
    auto m = t.lookup(dst);
    EXPECT_EQ(7, m.real_fd);
    EXPECT_EQ(socksdirect::kSocket, m.type);
    // Refcount on real_fd=7 is 2 (src + dst).
    EXPECT_EQ(2, t.refcount(src));
    EXPECT_EQ(2, t.refcount(dst));
}

TEST(FdRemap, DupToSelfIsNoOp) {
    FdRemapTable t;
    int v = t.alloc(socksdirect::kSocket, 7);
    int prev_refs = -1;
    auto prev = t.dup_to(v, v, &prev_refs);
    EXPECT_EQ(socksdirect::kUnknown, prev.type);  // nothing to close
    EXPECT_EQ(1, prev_refs);                      // refcount unchanged
    EXPECT_EQ(1, t.refcount(v));
}

TEST(FdRemap, DupToWithUnboundDstStillCreatesMapping) {
    FdRemapTable t;
    int src = t.alloc(socksdirect::kSocket, 7);
    int prev_refs = -1;
    auto prev = t.dup_to(src, 100, &prev_refs);
    EXPECT_EQ(socksdirect::kUnknown, prev.type);
    EXPECT_EQ(-1, prev_refs);  // dst was unmapped
    EXPECT_EQ(7, t.lookup(100).real_fd);
    EXPECT_EQ(2, t.refcount(src));
}

TEST(FdRemap, ReverseLookupSurvivesDupAndPartialFree) {
    FdRemapTable t;
    int v1 = t.alloc(socksdirect::kSocket, 9);
    int v2 = t.dup(v1);
    EXPECT_EQ(v1, t.reverse_lookup(socksdirect::kSocket, 9));
    // After freeing v1, the reverse map must still resolve to v2.
    int rc = t.free(v1);
    EXPECT_EQ(1, rc);
    int back = t.reverse_lookup(socksdirect::kSocket, 9);
    EXPECT_EQ(v2, back);
    // Now drop the last ref.
    rc = t.free(v2);
    EXPECT_EQ(0, rc);
    EXPECT_EQ(-1, t.reverse_lookup(socksdirect::kSocket, 9));
}

TEST(FdRemap, ConcurrentDupsAreSafe) {
    FdRemapTable t;
    int base = t.alloc(socksdirect::kSocket, 5);
    constexpr int kThreads = 8;
    constexpr int kPer = 200;
    std::atomic<int> ready{0};
    std::vector<std::vector<int>> per_thread(kThreads);
    std::vector<std::thread> ts;
    for (int n = 0; n < kThreads; ++n) {
        ts.emplace_back([&, n]() {
            ++ready;
            while (ready.load() < kThreads) std::this_thread::yield();
            for (int i = 0; i < kPer; ++i) {
                int v = t.dup(base);
                ASSERT_GE(v, 0);
                per_thread[n].push_back(v);
            }
        });
    }
    for (auto& th : ts) th.join();
    // Total refcount should be 1 + kThreads * kPer.
    EXPECT_EQ(1 + kThreads * kPer, t.refcount(base));
    // Free everything.
    for (auto& v : per_thread)
        for (int vfd : v) t.free(vfd);
    EXPECT_EQ(0, t.free(base));
}
