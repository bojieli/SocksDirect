// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::ShmSegment — POSIX-shm-backed SHM segment for one
// libsd connection pair.
//
// Layout:
//
//   +------------------------------------------------+
//   | sd_shm_header (page 0)                          |
//   |   magic, abi_major, abi_minor                   |
//   |   creator_pid, joiner_pid, refcount             |
//   |   ring_size                                     |
//   +------------------------------------------------+
//   | rings[0]: creator -> joiner (ShmRing<...>)      |
//   +------------------------------------------------+
//   | rings[1]: joiner -> creator (ShmRing<...>)      |
//   +------------------------------------------------+
//
// The creator (whichever side calls `open(create=true, key)` first)
// allocates and initializes the header + both rings. The joiner just
// `open(create=false, key)`s and uses the existing buffer.
//
// shm_open name format: "/sd-<32-bit hex of key>". Per Linux POSIX
// shm semantics, this corresponds to a file under /dev/shm/.
//
// We don't use shmget/shmat (System-V) because POSIX shm is more
// container-friendly (one bind-mount of /dev/shm versus Sys-V key
// space contention) and matches how the SocksDirect prototype's
// successor designs (eRPC, mTCP-shm) do it.
//
// Lifecycle:
//
//   ShmSegment seg;
//   seg.open(key, /*create=*/true);
//   // ... use seg.ring_outbound() and seg.ring_inbound() ...
//   seg.close();      // unmap; doesn't unlink
//   ShmSegment::unlink_by_key(key);  // last side calls this
//
// Reference counting:
//   The header has a refcount that both sides increment on open and
//   decrement on close. When it hits 0, whoever observed the
//   transition calls unlink_by_key. This handles the case where the
//   creator dies before the joiner — the joiner's close still
//   unlinks.

#ifndef SOCKSDIRECT_SHM_SEGMENT_HPP_
#define SOCKSDIRECT_SHM_SEGMENT_HPP_

#include "socksdirect/shm_ring.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace socksdirect {

constexpr std::uint32_t kShmSegmentMagic = 0x53444D31u;  // "SDM1"
using ShmSegmentRing = ShmRing<(1u << 20)>;              // 1 MiB per direction

struct sd_shm_header {
    std::atomic<std::uint32_t> magic;
    std::uint16_t              abi_major;
    std::uint16_t              abi_minor;
    std::atomic<std::int32_t>  refcount;
    std::int32_t               creator_pid;
    std::int32_t               joiner_pid;
    std::uint32_t              ring_size;
    std::uint8_t               _pad[40];   // round to 64 bytes
};
static_assert(sizeof(sd_shm_header) == 64, "header must be one cache line");

class ShmSegment {
public:
    enum Role { kRoleCreator, kRoleJoiner };

    ShmSegment() = default;
    ~ShmSegment() { close(); }
    ShmSegment(const ShmSegment&)            = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;
    ShmSegment(ShmSegment&& o) noexcept { *this = std::move(o); }
    ShmSegment& operator=(ShmSegment&& o) noexcept {
        if (this != &o) {
            close();
            base_   = o.base_;     o.base_   = nullptr;
            size_   = o.size_;     o.size_   = 0;
            key_    = o.key_;      o.key_    = 0;
            role_   = o.role_;     o.role_   = kRoleCreator;
            owned_  = o.owned_;    o.owned_  = false;
        }
        return *this;
    }

    // Open or create the segment identified by `key`. On success:
    //   - base_ points to the mmap'd region (size = layout_bytes()).
    //   - The header is verified.
    //   - refcount has been incremented.
    // Returns 0 on success; -1 on failure with errno set.
    int open(std::uint64_t key, Role role) {
        key_   = key;
        role_  = role;
        std::string name = name_for_key(key);
        // Sanity: atomic<u64> must be lock-free (we couldn't static_assert
        // this in C++11, see shm_ring.hpp).
        std::atomic<std::uint64_t> probe(0);
        if (!probe.is_lock_free()) { errno = ENOTSUP; return -1; }

        int oflag = (role == kRoleCreator) ? (O_RDWR | O_CREAT | O_EXCL)
                                           : (O_RDWR);
        int fd = ::shm_open(name.c_str(), oflag, 0660);
        if (fd < 0 && role == kRoleCreator && errno == EEXIST) {
            // A previous run leaked the segment. Unlink and retry.
            ::shm_unlink(name.c_str());
            fd = ::shm_open(name.c_str(), oflag, 0660);
        }
        // The monitor may answer "you're the joiner" before the
        // creator has reached shm_open. Spin briefly so the joiner
        // doesn't have to start out failing. ~5 s upper bound; if
        // the creator never shows up, return -ENOENT and the caller
        // falls back to TCP.
        if (fd < 0 && role == kRoleJoiner && errno == ENOENT) {
            for (int i = 0; i < 5000 && fd < 0; ++i) {
                ::usleep(1000);
                fd = ::shm_open(name.c_str(), oflag, 0660);
                if (fd >= 0) break;
                if (errno != ENOENT) break;
            }
        }
        if (fd < 0) return -1;

        if (role == kRoleCreator) {
            if (::ftruncate(fd, layout_bytes()) < 0) {
                int e = errno; ::close(fd); ::shm_unlink(name.c_str());
                errno = e; return -1;
            }
        } else {
            // Wait briefly for the creator to ftruncate (~1 ms in practice).
            for (int i = 0; i < 100; ++i) {
                struct stat st;
                if (::fstat(fd, &st) == 0 &&
                    static_cast<std::size_t>(st.st_size) >= layout_bytes()) break;
                ::usleep(1000);
            }
        }

        void* p = ::mmap(nullptr, layout_bytes(),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return -1;

        base_ = p;
        size_ = layout_bytes();
        owned_ = true;

        sd_shm_header* h = header();
        if (role == kRoleCreator) {
            std::memset(h, 0, sizeof(*h));
            h->abi_major = 1;
            h->abi_minor = 0;
            h->ring_size = static_cast<std::uint32_t>(ShmSegmentRing::kCapacity);
            h->creator_pid = static_cast<std::int32_t>(::getpid());
            h->refcount.store(1, std::memory_order_relaxed);
            ring_creator_to_joiner()->init();
            ring_joiner_to_creator()->init();
            // Magic last so a joiner reading the header concurrently
            // either sees the old value (and retries) or the fully
            // initialized header.
            h->magic.store(kShmSegmentMagic, std::memory_order_release);
        } else {
            // Spin until the creator has finished init.
            for (int i = 0; i < 100; ++i) {
                if (h->magic.load(std::memory_order_acquire) == kShmSegmentMagic)
                    break;
                ::usleep(1000);
            }
            if (h->magic.load(std::memory_order_acquire) != kShmSegmentMagic) {
                ::munmap(base_, size_); base_ = nullptr;
                errno = EPROTO; return -1;
            }
            if (h->abi_major != 1) {
                ::munmap(base_, size_); base_ = nullptr;
                errno = ENOTSUP; return -1;
            }
            h->joiner_pid = static_cast<std::int32_t>(::getpid());
            h->refcount.fetch_add(1, std::memory_order_acq_rel);
        }
        return 0;
    }

    void close() {
        if (!base_) return;
        sd_shm_header* h = header();
        // Mark our outbound ring closed so the peer's recv eventually
        // returns 0.
        if (role_ == kRoleCreator) ring_creator_to_joiner()->mark_closed();
        else                       ring_joiner_to_creator()->mark_closed();
        std::int32_t prev = h->refcount.fetch_sub(1, std::memory_order_acq_rel);
        bool last = (prev == 1);
        ::munmap(base_, size_);
        base_ = nullptr;
        if (last) {
            std::string name = name_for_key(key_);
            ::shm_unlink(name.c_str());
        }
        owned_ = false;
    }

    static int unlink_by_key(std::uint64_t key) {
        return ::shm_unlink(name_for_key(key).c_str());
    }

    sd_shm_header* header() {
        return reinterpret_cast<sd_shm_header*>(base_);
    }

    // Outbound ring for *this* role (i.e. the one we send through).
    ShmSegmentRing* ring_outbound() {
        return role_ == kRoleCreator ? ring_creator_to_joiner()
                                     : ring_joiner_to_creator();
    }
    // Inbound ring for this role (the one we recv from).
    ShmSegmentRing* ring_inbound() {
        return role_ == kRoleCreator ? ring_joiner_to_creator()
                                     : ring_creator_to_joiner();
    }

    bool is_open() const { return base_ != nullptr; }
    Role role() const { return role_; }
    std::uint64_t key() const { return key_; }

    static std::string name_for_key(std::uint64_t key) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "/sd-%016llx",
                      static_cast<unsigned long long>(key));
        return buf;
    }

    static std::size_t layout_bytes() {
        // Header is one cache line; rings are placed on cache-line
        // boundaries. We round to a page so munmap doesn't surprise us.
        std::size_t want = sizeof(sd_shm_header) + 2 * sizeof(ShmSegmentRing);
        std::size_t page = 4096;
        return (want + page - 1) & ~(page - 1);
    }

private:
    ShmSegmentRing* ring_creator_to_joiner() {
        return reinterpret_cast<ShmSegmentRing*>(
            static_cast<char*>(base_) + sizeof(sd_shm_header));
    }
    ShmSegmentRing* ring_joiner_to_creator() {
        return reinterpret_cast<ShmSegmentRing*>(
            static_cast<char*>(base_) + sizeof(sd_shm_header)
            + sizeof(ShmSegmentRing));
    }

    void*         base_  = nullptr;
    std::size_t   size_  = 0;
    std::uint64_t key_   = 0;
    Role          role_  = kRoleCreator;
    bool          owned_ = false;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_SHM_SEGMENT_HPP_
