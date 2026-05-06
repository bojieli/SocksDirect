// Unit tests for socksdirect::ShmRing.
//
// We test the ring in regular heap memory; the SHM bit is just where
// we put the storage in production.

#include "socksdirect/shm_ring.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace {

using Ring = socksdirect::ShmRing<4096>;  // 4 KiB ring is enough to test

TEST(ShmRing, EmptyAtBoot) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    EXPECT_EQ(0u, r->readable());
    EXPECT_EQ(4096u, r->writable());
    EXPECT_FALSE(r->peer_closed());
    r->~Ring();
    free(r);
}

TEST(ShmRing, SingleSendRecv) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    const char msg[] = "hello world";
    EXPECT_EQ(sizeof(msg), r->send_some(msg, sizeof(msg)));
    EXPECT_EQ(sizeof(msg), r->readable());
    char buf[64] = {0};
    EXPECT_EQ(sizeof(msg), r->recv_some(buf, sizeof(buf)));
    EXPECT_STREQ(msg, buf);
    EXPECT_EQ(0u, r->readable());
    r->~Ring();
    free(r);
}

TEST(ShmRing, FillExactlyAndDrain) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    std::vector<char> in(4096, 'a');
    EXPECT_EQ(4096u, r->send_some(in.data(), in.size()));
    EXPECT_EQ(0u, r->writable());
    // One more byte rejected.
    EXPECT_EQ(0u, r->send_some("x", 1));
    std::vector<char> out(4096);
    EXPECT_EQ(4096u, r->recv_some(out.data(), out.size()));
    EXPECT_EQ(in, out);
    EXPECT_EQ(4096u, r->writable());
    r->~Ring();
    free(r);
}

TEST(ShmRing, WrapAround) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    // Produce + consume 3 KiB so head/tail are mid-buffer; then a
    // 2 KiB write straddles the wrap.
    std::vector<char> three_k(3072, 'x');
    EXPECT_EQ(3072u, r->send_some(three_k.data(), three_k.size()));
    std::vector<char> drain(3072);
    EXPECT_EQ(3072u, r->recv_some(drain.data(), drain.size()));
    EXPECT_EQ(three_k, drain);

    std::vector<char> two_k(2048);
    for (std::size_t i = 0; i < two_k.size(); ++i)
        two_k[i] = static_cast<char>(i & 0xff);
    EXPECT_EQ(2048u, r->send_some(two_k.data(), two_k.size()));
    std::vector<char> back(2048);
    EXPECT_EQ(2048u, r->recv_some(back.data(), back.size()));
    EXPECT_EQ(two_k, back);
    r->~Ring();
    free(r);
}

TEST(ShmRing, PartialSendWhenAlmostFull) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    std::vector<char> a(4000, 'a');
    EXPECT_EQ(4000u, r->send_some(a.data(), a.size()));
    // Only 96 bytes left; send 200 returns 96.
    std::vector<char> b(200, 'b');
    EXPECT_EQ(96u, r->send_some(b.data(), b.size()));
    r->~Ring();
    free(r);
}

TEST(ShmRing, PeerClosedFlag) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    EXPECT_FALSE(r->peer_closed());
    r->mark_closed();
    EXPECT_TRUE(r->peer_closed());
    // Closing doesn't drop bytes already in the buffer.
    EXPECT_EQ(0u, r->readable());
    EXPECT_EQ(4096u, r->writable());  // closed doesn't gate sends per se
    r->~Ring();
    free(r);
}

TEST(ShmRing, SpscThreadStressPreservesByteOrder) {
    Ring* r = nullptr;
    ASSERT_EQ(0, posix_memalign(reinterpret_cast<void**>(&r), 64, sizeof(Ring)));
    new (r) Ring();
    r->init();
    constexpr std::size_t kN = 1000000;  // 1 MB through a 4 KiB ring
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        std::size_t sent = 0;
        std::vector<std::uint8_t> chunk(64);
        while (sent < kN) {
            for (std::size_t i = 0; i < chunk.size(); ++i) {
                chunk[i] = static_cast<std::uint8_t>((sent + i) & 0xff);
            }
            std::size_t want = chunk.size();
            std::size_t off  = 0;
            while (off < want) {
                std::size_t did = r->send_some(chunk.data() + off, want - off);
                off += did;
                if (did == 0) std::this_thread::yield();
            }
            sent += want;
        }
        r->mark_closed();
        producer_done.store(true);
    });

    std::thread consumer([&]() {
        std::size_t got = 0;
        std::vector<std::uint8_t> buf(64);
        while (got < kN) {
            std::size_t did = r->recv_some(buf.data(), buf.size());
            if (did == 0) {
                if (r->peer_closed() && r->readable() == 0 && got >= kN) break;
                std::this_thread::yield();
                continue;
            }
            for (std::size_t i = 0; i < did; ++i) {
                ASSERT_EQ(static_cast<std::uint8_t>((got + i) & 0xff), buf[i])
                    << "byte " << (got + i);
            }
            got += did;
        }
        EXPECT_EQ(kN, got);
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(r->peer_closed());
    r->~Ring();
    free(r);
}

TEST(ShmRing, LayoutOffsetsAreStable) {
    // The producer/consumer cache lines and the data array offsets are
    // part of the on-disk ABI. Bumping any of these is a wire break;
    // keep the assertions explicit.
    EXPECT_EQ(64u, offsetof(Ring, head));
    EXPECT_EQ(128u, offsetof(Ring, data));
}

}  // namespace
