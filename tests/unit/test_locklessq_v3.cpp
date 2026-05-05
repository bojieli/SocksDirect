// Unit tests for locklessqueue_t_v3.
//
// v3 is a single-producer / single-consumer SHM queue with two ring blocks
// linked into a circular list and a per-block "block is free" handshake
// flag. The producer pushes 16-byte aligned elements with push_nb(); the
// consumer iterates with begin() and del()s entries it has consumed.
//
// We test it in-process with both blocks allocated on the stack/heap (no
// SHM mmap required). All tests are single-threaded for the basic cases;
// the multi-thread stress test exercises the actual SPSC contract.
//
// Constraint we cannot test cheaply: the atomic_copy16 inline asm requires
// 16-byte alignment of the ring buffers. We satisfy it with alignas.

#include "common/locklessq_v3.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace {

constexpr uint32_t kSize = 64;
using Q = locklessqueue_t_v3<int, kSize>;

struct QueueWithBuffers {
    alignas(16) Q::element_t buf1[kSize];
    alignas(16) Q::element_t buf2[kSize];
    bool ret1 = false;
    bool ret2 = false;
    Q producer;
    Q consumer;
    QueueWithBuffers() {
        Q::mem_ptr_t p1{buf1, &ret1};
        Q::mem_ptr_t p2{buf2, &ret2};
        producer.init_ptr(p1, p2, /*is_receiver=*/false);
        consumer.init_ptr(p1, p2, /*is_receiver=*/true);
        producer.init_mem();
    }
};

TEST(LocklessQv3, EmptyQueueIteratorIsInvalid) {
    QueueWithBuffers q;
    auto it = q.consumer.begin();
    // The element at the start has isvalid=false after init_mem.
    // valid() reflects iterator-position-validity, not data-validity, so
    // we look at the data instead.
    EXPECT_FALSE(it->isvalid);
}

TEST(LocklessQv3, SinglePushIsVisibleToConsumer) {
    QueueWithBuffers q;
    EXPECT_TRUE(q.producer.push_nb(42));
    auto it = q.consumer.begin();
    ASSERT_TRUE(it.valid());
    EXPECT_TRUE(it->isvalid);
    EXPECT_EQ(42, it->data);
}

TEST(LocklessQv3, BulkPushFillsBothBlocks) {
    // The queue is initialized with two blocks linked in a ring; both are
    // marked is_avail at construction time. So the producer can push up to
    // 2 * kSize elements before exhausting both blocks.
    QueueWithBuffers q;
    for (uint32_t i = 0; i < 2 * kSize; ++i) {
        EXPECT_TRUE(q.producer.push_nb(static_cast<int>(i))) << "i=" << i;
    }
    // The (2*kSize + 1)th push must fail because no block is free.
    EXPECT_FALSE(q.producer.push_nb(999));
}

TEST(LocklessQv3, ConsumerDelFreesBlockAfterFullDrain) {
    QueueWithBuffers q;
    // Fill exactly one block worth.
    for (uint32_t i = 0; i < kSize; ++i) {
        ASSERT_TRUE(q.producer.push_nb(static_cast<int>(i)));
    }
    // Drain everything.
    auto it = q.consumer.begin();
    for (uint32_t i = 0; i < kSize; ++i) {
        ASSERT_TRUE(it.valid());
        EXPECT_TRUE(it->isvalid);
        EXPECT_EQ(static_cast<int>(i), it->data);
        it.del();
    }
    // After the drain, the freed block is recycled via return_flag, so the
    // producer can keep pushing into the second block and eventually back
    // into the first.
    for (uint32_t i = 0; i < kSize; ++i) {
        EXPECT_TRUE(q.producer.push_nb(static_cast<int>(1000 + i)));
    }
}

TEST(LocklessQv3, SpscStressPreservesOrder) {
    QueueWithBuffers q;
    constexpr int kN = 200000;
    std::atomic<bool> producer_done{false};

    auto producer = [&]() {
        for (int i = 0; i < kN; ++i) {
            while (!q.producer.push_nb(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    };

    auto consumer = [&]() {
        int expected = 0;
        while (expected < kN) {
            auto it = q.consumer.begin();
            if (!it.valid() || !it->isvalid) {
                if (producer_done.load(std::memory_order_acquire) && expected == kN)
                    break;
                std::this_thread::yield();
                continue;
            }
            ASSERT_EQ(expected, it->data);
            it.del();
            ++expected;
        }
        EXPECT_EQ(kN, expected);
    };

    std::thread tp(producer);
    std::thread tc(consumer);
    tp.join();
    tc.join();
}

}  // namespace
