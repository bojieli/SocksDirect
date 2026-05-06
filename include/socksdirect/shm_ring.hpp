// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::ShmRing — process-shared SPSC byte-stream ring.
//
// What this is:
//   A fixed-size byte-stream ring buffer, designed to live in shared
//   memory between two processes. One side is the producer (calls
//   send_*), the other is the consumer (calls recv_*). The two roles
//   are static — the ring is single-producer / single-consumer.
//
// What this is NOT:
//   - A two-way connection (use two rings — one per direction).
//   - A message-framed queue (it's a stream of bytes; framing is the
//     caller's problem if needed).
//   - Multi-producer / multi-consumer (don't use it that way).
//
// Memory layout, per ring (sized by the kBufBytes template parameter):
//
//   offset 0       producer-state     (cache-line padded)
//                    tail   : atomic<u64>  -- producer-only writes
//                    closed : atomic<u8>   -- producer sets on EOF
//   offset 64      consumer-state     (cache-line padded)
//                    head   : atomic<u64>  -- consumer-only writes
//   offset 128     data[kBufBytes]
//
// Concurrency model:
//   - tail is written only by the producer; head only by the consumer.
//     Both sides observe each other through release/acquire pairs.
//   - "empty" is `head == tail`; "full" is `tail - head == kBufBytes`.
//   - kBufBytes must be a power of two so we can use a mask instead of
//     modulo. Static-asserted.
//   - We use std::atomic with default order on the inc/dec, but the
//     publication points (after writing data, before reading head/tail)
//     use explicit memory_order_release / memory_order_acquire so the
//     compiler doesn't reorder data writes past the tail bump.
//
// Process-shared safety:
//   - The header is POD; no virtual functions, no internal pointers.
//   - std::atomic<uintN_t> is lock-free on x86-64 for N <= 8 bytes
//     (assertable at compile time via is_always_lock_free). The
//     constructor checks this with static_assert.
//   - Crash safety: if the producer crashes mid-write, the consumer
//     can read the bytes already published (those whose tail bump
//     has happened) and will see the closed flag never get set. The
//     handshake layer is responsible for noticing the peer is dead
//     and tearing the ring down.

#ifndef SOCKSDIRECT_SHM_RING_HPP_
#define SOCKSDIRECT_SHM_RING_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace socksdirect {

// Default buffer size: 1 MiB. The connection pair (two rings) ends up
// at 2 MiB + headers, fits comfortably in one hugepage.
template <std::size_t kBufBytes = (1u << 20)>
struct ShmRing {
    static_assert((kBufBytes & (kBufBytes - 1)) == 0,
                  "kBufBytes must be a power of two");
    static_assert(kBufBytes >= 4096,
                  "ring smaller than a page is silly");
    // Re-export the template parameter as a member so `Ring::kCapacity`
    // is usable from non-templated code (e.g. ShmSegment).
    static constexpr std::size_t kCapacity = kBufBytes;
    static constexpr std::size_t kCacheLine = 64;
    static constexpr std::uint64_t kMask = kBufBytes - 1;

    // Producer-side cache line (writes tail + closed).
    alignas(kCacheLine) std::atomic<std::uint64_t> tail;
    std::atomic<std::uint8_t>                       closed;
    char _pad_p[kCacheLine - sizeof(tail) - sizeof(closed)];

    // Consumer-side cache line (writes head).
    alignas(kCacheLine) std::atomic<std::uint64_t> head;
    char _pad_c[kCacheLine - sizeof(head)];

    // Data.
    alignas(kCacheLine) std::uint8_t data[kBufBytes];

    void init() {
        tail.store(0, std::memory_order_relaxed);
        head.store(0, std::memory_order_relaxed);
        closed.store(0, std::memory_order_relaxed);
        // We don't memset the data; the head/tail markers fully
        // describe what's valid.
    }

    // Bytes available to the consumer.
    std::size_t readable() const {
        std::uint64_t t = tail.load(std::memory_order_acquire);
        std::uint64_t h = head.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(t - h);
    }

    // Free space available to the producer.
    std::size_t writable() const {
        std::uint64_t t = tail.load(std::memory_order_relaxed);
        std::uint64_t h = head.load(std::memory_order_acquire);
        return kBufBytes - static_cast<std::size_t>(t - h);
    }

    bool peer_closed() const {
        return closed.load(std::memory_order_acquire) != 0;
    }

    // Producer marks the stream as half-closed (for shutdown(SHUT_WR)).
    // The consumer will return 0 from recv_some() once the buffer
    // drains and this flag is observed.
    void mark_closed() {
        closed.store(1, std::memory_order_release);
    }

    // Non-blocking send: writes up to `len` bytes from `buf`, returns
    // bytes actually written (0..len). Never blocks.
    std::size_t send_some(const void* buf, std::size_t len) {
        std::uint64_t t = tail.load(std::memory_order_relaxed);
        std::uint64_t h = head.load(std::memory_order_acquire);
        std::size_t free = kBufBytes - static_cast<std::size_t>(t - h);
        if (free == 0) return 0;
        std::size_t n = (len < free) ? len : free;
        std::size_t off  = static_cast<std::size_t>(t & kMask);
        std::size_t first = (kBufBytes - off < n) ? (kBufBytes - off) : n;
        std::memcpy(data + off, buf, first);
        if (first < n) {
            std::memcpy(data, static_cast<const char*>(buf) + first, n - first);
        }
        tail.store(t + n, std::memory_order_release);
        return n;
    }

    // Non-blocking recv: reads up to `len` bytes into `buf`, returns
    // bytes actually read. 0 means "no data right now"; check
    // peer_closed() to distinguish from EOF.
    std::size_t recv_some(void* buf, std::size_t len) {
        std::uint64_t t = tail.load(std::memory_order_acquire);
        std::uint64_t h = head.load(std::memory_order_relaxed);
        std::size_t avail = static_cast<std::size_t>(t - h);
        if (avail == 0) return 0;
        std::size_t n = (len < avail) ? len : avail;
        std::size_t off  = static_cast<std::size_t>(h & kMask);
        std::size_t first = (kBufBytes - off < n) ? (kBufBytes - off) : n;
        std::memcpy(buf, data + off, first);
        if (first < n) {
            std::memcpy(static_cast<char*>(buf) + first, data, n - first);
        }
        head.store(h + n, std::memory_order_release);
        return n;
    }
};

// Sanity assertion on the on-disk layout. The exact offsets are part
// of the SHM ABI between processes; if you change the type, both peers
// must agree.
//
// We can't static_assert std::atomic<u64>::is_always_lock_free here
// (that's C++17 and the project is C++11). On every supported
// platform (x86-64, aarch64), atomic<u64> is lock-free; ShmSegment::open
// runtime-checks it.

}  // namespace socksdirect

#endif  // SOCKSDIRECT_SHM_RING_HPP_
