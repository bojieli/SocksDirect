// SPDX-License-Identifier: Apache-2.0
//
// FdRemapTable: thread-safe mapping between user-facing "virtual" fds and
// the underlying real fds owned by either the kernel or libsd. Replaces
// the inline static-global implementation in lib/socket_lib.cpp.
//
// Contract (matches the prototype's behavior, which the production code
// will migrate to in Phase 3):
//   - alloc(type, real_fd): returns a virtual fd. Reuses freed virtual fds
//     before allocating new ones.
//   - lookup(virtual_fd): returns {type, real_fd}, or {kUnknown, -1} if not
//     mapped. Negative virtual_fd is passed through (errno-style sentinels).
//   - reverse_lookup(type, real_fd): returns the virtual fd that points to
//     this (type, real_fd), or -1 if there is no such mapping.
//   - free(virtual_fd): marks the virtual fd reusable. Idempotent on
//     already-free fds (logs nothing — caller decides).
//   - All operations are safe under concurrent calls from multiple threads.
//
// The allocation scheme is a free-list stack on top of a monotonically
// increasing watermark. This matches the prototype and keeps allocation
// O(1) without searching.
//
// Not yet supported (call out so callers don't ship bugs against this):
//   - Per-fd refcounting (needed for dup/dup2/dup3).
//   - Atomic alloc-and-set in one critical section.

#ifndef SOCKSDIRECT_FD_REMAP_HPP_
#define SOCKSDIRECT_FD_REMAP_HPP_

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace socksdirect {

enum FdType : int {
    kUnknown  = 0,
    kSystem   = 1,  // pass-through to kernel
    kSocket   = 2,  // accelerated TCP socket
    kEpoll    = 3,
    kEventfd  = 4,
    kFile     = 5,
    kNumTypes = 6,
};

struct FdMapping {
    FdType type;
    int real_fd;
};

class FdRemapTable {
public:
    // Initial table size; grows on demand.
    static constexpr std::size_t kInitialCapacity = 256;

    FdRemapTable() {
        forward_.resize(kInitialCapacity, FdMapping{kUnknown, -1});
        reverse_.assign(kNumTypes, std::vector<int>(kInitialCapacity, -1));
    }

    // Allocate a virtual fd for (type, real_fd). Returns the virtual fd.
    // Negative real_fd is returned unchanged (the caller is propagating an
    // errno from a wrapped libc call).
    int alloc(FdType type, int real_fd) {
        if (real_fd < 0) return real_fd;
        int vfd;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!free_list_.empty()) {
                vfd = free_list_.back();
                free_list_.pop_back();
            } else {
                vfd = static_cast<int>(watermark_++);
            }
            ensure_capacity_locked(vfd);
            forward_[vfd] = FdMapping{type, real_fd};
            if (type != kUnknown) {
                ensure_reverse_capacity_locked(type, real_fd);
                reverse_[type][real_fd] = vfd;
            }
        }
        return vfd;
    }

    // Bind an explicit virtual_fd to (type, real_fd). Used when reproducing a
    // mapping in a child after fork. Caller is responsible for ensuring the
    // virtual_fd is not currently allocated to something else.
    void set(int virtual_fd, FdType type, int real_fd) {
        if (virtual_fd < 0 || real_fd < 0) return;
        std::lock_guard<std::mutex> g(mu_);
        ensure_capacity_locked(virtual_fd);
        forward_[virtual_fd] = FdMapping{type, real_fd};
        if (type != kUnknown) {
            ensure_reverse_capacity_locked(type, real_fd);
            reverse_[type][real_fd] = virtual_fd;
        }
        if (static_cast<std::size_t>(virtual_fd) >= watermark_) {
            watermark_ = static_cast<std::size_t>(virtual_fd) + 1;
        }
    }

    FdMapping lookup(int virtual_fd) const {
        if (virtual_fd < 0) return FdMapping{kUnknown, -1};
        std::lock_guard<std::mutex> g(mu_);
        if (static_cast<std::size_t>(virtual_fd) >= forward_.size())
            return FdMapping{kUnknown, -1};
        return forward_[virtual_fd];
    }

    int reverse_lookup(FdType type, int real_fd) const {
        if (real_fd < 0 || type < 0 || type >= kNumTypes) return -1;
        std::lock_guard<std::mutex> g(mu_);
        const auto& r = reverse_[type];
        if (static_cast<std::size_t>(real_fd) >= r.size()) return -1;
        return r[real_fd];
    }

    void free(int virtual_fd) {
        if (virtual_fd < 0) return;
        std::lock_guard<std::mutex> g(mu_);
        if (static_cast<std::size_t>(virtual_fd) >= forward_.size()) return;
        FdMapping cur = forward_[virtual_fd];
        if (cur.type == kUnknown && cur.real_fd == -1) {
            // Already free; idempotent.
            return;
        }
        forward_[virtual_fd] = FdMapping{kUnknown, -1};
        if (cur.type != kUnknown && cur.real_fd >= 0
            && static_cast<std::size_t>(cur.real_fd) < reverse_[cur.type].size()
            && reverse_[cur.type][cur.real_fd] == virtual_fd) {
            reverse_[cur.type][cur.real_fd] = -1;
        }
        free_list_.push_back(virtual_fd);
    }

    std::size_t live_count() const {
        std::lock_guard<std::mutex> g(mu_);
        return watermark_ - free_list_.size();
    }

private:
    void ensure_capacity_locked(int virtual_fd) {
        std::size_t needed = static_cast<std::size_t>(virtual_fd) + 1;
        if (needed <= forward_.size()) return;
        std::size_t new_size = forward_.size();
        while (new_size < needed) new_size *= 2;
        forward_.resize(new_size, FdMapping{kUnknown, -1});
    }

    void ensure_reverse_capacity_locked(FdType type, int real_fd) {
        auto& r = reverse_[type];
        std::size_t needed = static_cast<std::size_t>(real_fd) + 1;
        if (needed <= r.size()) return;
        std::size_t new_size = r.size();
        while (new_size < needed) new_size *= 2;
        r.resize(new_size, -1);
    }

    mutable std::mutex mu_;
    std::vector<FdMapping> forward_;
    std::vector<std::vector<int>> reverse_;  // reverse_[type][real_fd] = vfd
    std::vector<int> free_list_;
    std::size_t watermark_ = 0;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_FD_REMAP_HPP_
