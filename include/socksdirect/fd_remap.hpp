// SPDX-License-Identifier: Apache-2.0
//
// FdRemapTable: thread-safe mapping between user-facing "virtual" fds and
// the underlying real fds owned by either the kernel or libsd. Replaces
// the inline static-global implementation in lib/socket_lib.cpp.
//
// Contract:
//   - alloc(type, real_fd): returns a fresh virtual fd. Reuses freed
//     virtual fds before allocating new ones. Each alloc starts the
//     refcount on the underlying (type, real_fd) at 1 if it was 0,
//     otherwise increments it (so a second alloc against the same
//     real_fd returns a different vfd that shares the same underlying
//     resource).
//   - lookup(virtual_fd): returns {type, real_fd}, or {kUnknown, -1}.
//   - reverse_lookup(type, real_fd): returns *some* vfd that points to
//     this (type, real_fd), or -1. There may be multiple vfds (after
//     dup); we return the first one we registered.
//   - dup(virtual_fd): allocate a fresh vfd that points at the same
//     (type, real_fd) and bumps its refcount. Returns -1 with errno
//     set on failure.
//   - dup_to(virtual_fd, target_vfd): like dup2 — make `target_vfd`
//     point at virtual_fd's underlying resource. If `target_vfd` was
//     previously bound, that binding's refcount drops first. If
//     `virtual_fd == target_vfd`, no-op (matches dup2 semantics).
//   - free(virtual_fd): decrements the underlying refcount; the
//     real_fd's reverse mapping is cleared only when the count hits 0.
//     Returns the post-decrement count (or 0 if vfd was already free).
//   - All operations are safe under concurrent calls from multiple
//     threads.
//
// dup/dup2/dup3 in src/lib/ are layered on top: alloc real fd via
// FdRemapTable::dup or dup_to; libsd's caller owns the lifecycle of
// the real fd (close it once when refcount transitions to 0).
//
// The allocation scheme is a free-list stack on top of a monotonically
// increasing watermark. Allocation is O(1) without searching.
//
// Refcount details:
//   - Stored in refcount_[type][real_fd] alongside the reverse map.
//   - Resized in lockstep with reverse_[type] so the indexing matches.

#ifndef SOCKSDIRECT_FD_REMAP_HPP_
#define SOCKSDIRECT_FD_REMAP_HPP_

#include <cerrno>
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
    static constexpr std::size_t kInitialCapacity = 256;

    FdRemapTable() {
        forward_.resize(kInitialCapacity, FdMapping{kUnknown, -1});
        reverse_.assign(kNumTypes, std::vector<int>(kInitialCapacity, -1));
        refcount_.assign(kNumTypes, std::vector<int>(kInitialCapacity, 0));
    }

    // Allocate a virtual fd for (type, real_fd). Returns the virtual fd.
    // Bumps the underlying refcount; the caller is responsible for closing
    // real_fd exactly once when the last vfd freeing transitions the
    // refcount to 0 (use `free()` and check the return).
    int alloc(FdType type, int real_fd) {
        if (real_fd < 0) return real_fd;
        int vfd;
        {
            std::lock_guard<std::mutex> g(mu_);
            vfd = alloc_locked();
            forward_[vfd] = FdMapping{type, real_fd};
            if (type != kUnknown) {
                ensure_reverse_capacity_locked(type, real_fd);
                if (reverse_[type][real_fd] < 0) {
                    reverse_[type][real_fd] = vfd;
                }
                ++refcount_[type][real_fd];
            }
        }
        return vfd;
    }

    // Bind an explicit virtual_fd to (type, real_fd). Used when reproducing
    // a mapping in a child after fork. Caller is responsible for ensuring
    // the virtual_fd is not currently allocated to something else.
    void set(int virtual_fd, FdType type, int real_fd) {
        if (virtual_fd < 0 || real_fd < 0) return;
        std::lock_guard<std::mutex> g(mu_);
        ensure_capacity_locked(virtual_fd);
        FdMapping prev = forward_[virtual_fd];
        if (prev.type != kUnknown && prev.real_fd >= 0) {
            decref_locked(prev.type, prev.real_fd, virtual_fd);
        }
        forward_[virtual_fd] = FdMapping{type, real_fd};
        if (type != kUnknown) {
            ensure_reverse_capacity_locked(type, real_fd);
            if (reverse_[type][real_fd] < 0) {
                reverse_[type][real_fd] = virtual_fd;
            }
            ++refcount_[type][real_fd];
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

    // Refcount the (type, real_fd) backing virtual_fd has. Returns 0 if
    // virtual_fd is unmapped.
    int refcount(int virtual_fd) const {
        if (virtual_fd < 0) return 0;
        std::lock_guard<std::mutex> g(mu_);
        if (static_cast<std::size_t>(virtual_fd) >= forward_.size()) return 0;
        FdMapping m = forward_[virtual_fd];
        if (m.type == kUnknown || m.real_fd < 0) return 0;
        if (static_cast<std::size_t>(m.real_fd) >= refcount_[m.type].size()) return 0;
        return refcount_[m.type][m.real_fd];
    }

    // dup: allocate a fresh vfd pointing at virtual_fd's resource.
    // Bumps the refcount. Returns -1 + errno=EBADF if virtual_fd is
    // unmapped. Mirrors dup(2) semantics (the new vfd does not inherit
    // O_CLOEXEC).
    int dup(int virtual_fd) {
        std::lock_guard<std::mutex> g(mu_);
        if (virtual_fd < 0
            || static_cast<std::size_t>(virtual_fd) >= forward_.size()
            || forward_[virtual_fd].type == kUnknown
            || forward_[virtual_fd].real_fd < 0) {
            errno = EBADF;
            return -1;
        }
        FdMapping src = forward_[virtual_fd];
        int new_vfd = alloc_locked();
        forward_[new_vfd] = src;
        ensure_reverse_capacity_locked(src.type, src.real_fd);
        ++refcount_[src.type][src.real_fd];
        // The reverse map already points at the original vfd; we don't
        // overwrite it.
        return new_vfd;
    }

    // dup_to: dup2 semantics. If src and dst are the same and src is
    // valid, returns dst (no-op). Otherwise drops dst's old binding,
    // points dst at src's resource, bumps refcount.
    //
    // Returns the *previous* (type, real_fd) at dst so the caller can
    // close the underlying real fd if its refcount hit 0. The previous
    // refcount-after-decrement is in `out_prev_refs`. On failure
    // returns {kUnknown, -1} and sets errno.
    FdMapping dup_to(int src_vfd, int dst_vfd, int* out_prev_refs = nullptr) {
        if (out_prev_refs) *out_prev_refs = -1;
        if (src_vfd < 0 || dst_vfd < 0) {
            errno = EBADF;
            return FdMapping{kUnknown, -1};
        }
        std::lock_guard<std::mutex> g(mu_);
        if (static_cast<std::size_t>(src_vfd) >= forward_.size()
            || forward_[src_vfd].type == kUnknown
            || forward_[src_vfd].real_fd < 0) {
            errno = EBADF;
            return FdMapping{kUnknown, -1};
        }
        FdMapping src = forward_[src_vfd];
        if (src_vfd == dst_vfd) {
            if (out_prev_refs) *out_prev_refs = refcount_[src.type][src.real_fd];
            return FdMapping{kUnknown, -1};  // no-op; nothing to close
        }
        ensure_capacity_locked(dst_vfd);
        FdMapping prev = forward_[dst_vfd];
        int prev_refs = -1;
        if (prev.type != kUnknown && prev.real_fd >= 0) {
            prev_refs = decref_locked(prev.type, prev.real_fd, dst_vfd);
        }
        forward_[dst_vfd] = src;
        ensure_reverse_capacity_locked(src.type, src.real_fd);
        if (reverse_[src.type][src.real_fd] < 0) {
            reverse_[src.type][src.real_fd] = dst_vfd;
        }
        ++refcount_[src.type][src.real_fd];
        if (static_cast<std::size_t>(dst_vfd) >= watermark_) {
            watermark_ = static_cast<std::size_t>(dst_vfd) + 1;
        }
        if (out_prev_refs) *out_prev_refs = prev_refs;
        return prev;
    }

    // free: drop one reference on virtual_fd. Returns the *post-decrement*
    // refcount of the underlying (type, real_fd). 0 means the caller may
    // close(real_fd). Idempotent on already-freed vfds (returns 0).
    int free(int virtual_fd) {
        if (virtual_fd < 0) return 0;
        std::lock_guard<std::mutex> g(mu_);
        if (static_cast<std::size_t>(virtual_fd) >= forward_.size()) return 0;
        FdMapping cur = forward_[virtual_fd];
        if (cur.type == kUnknown && cur.real_fd == -1) return 0;
        forward_[virtual_fd] = FdMapping{kUnknown, -1};
        free_list_.push_back(virtual_fd);
        if (cur.type == kUnknown || cur.real_fd < 0) return 0;
        return decref_locked(cur.type, cur.real_fd, virtual_fd);
    }

    std::size_t live_count() const {
        std::lock_guard<std::mutex> g(mu_);
        return watermark_ - free_list_.size();
    }

private:
    int alloc_locked() {
        int vfd;
        if (!free_list_.empty()) {
            vfd = free_list_.back();
            free_list_.pop_back();
        } else {
            vfd = static_cast<int>(watermark_++);
        }
        ensure_capacity_locked(vfd);
        return vfd;
    }

    int decref_locked(FdType type, int real_fd, int vfd_being_dropped) {
        if (type == kUnknown || real_fd < 0) return 0;
        if (static_cast<std::size_t>(real_fd) >= refcount_[type].size()) return 0;
        int& rc = refcount_[type][real_fd];
        if (rc <= 0) return 0;
        --rc;
        if (rc == 0) {
            if (reverse_[type][real_fd] == vfd_being_dropped) {
                reverse_[type][real_fd] = -1;
            }
        } else if (reverse_[type][real_fd] == vfd_being_dropped) {
            // We're dropping the vfd that the reverse map points at, but
            // other vfds still hold references. Find another live vfd
            // that points at this (type, real_fd). Linear scan is fine
            // for the common case of refcount <= 4 (typical dup count).
            int replacement = -1;
            for (std::size_t i = 0; i < forward_.size(); ++i) {
                if (forward_[i].type == type && forward_[i].real_fd == real_fd) {
                    replacement = static_cast<int>(i);
                    break;
                }
            }
            reverse_[type][real_fd] = replacement;
        }
        return rc;
    }

    void ensure_capacity_locked(int virtual_fd) {
        std::size_t needed = static_cast<std::size_t>(virtual_fd) + 1;
        if (needed <= forward_.size()) return;
        std::size_t new_size = forward_.size();
        while (new_size < needed) new_size *= 2;
        forward_.resize(new_size, FdMapping{kUnknown, -1});
    }

    void ensure_reverse_capacity_locked(FdType type, int real_fd) {
        auto& r = reverse_[type];
        auto& c = refcount_[type];
        std::size_t needed = static_cast<std::size_t>(real_fd) + 1;
        if (needed <= r.size()) return;
        std::size_t new_size = r.size();
        while (new_size < needed) new_size *= 2;
        r.resize(new_size, -1);
        c.resize(new_size, 0);
    }

    mutable std::mutex mu_;
    std::vector<FdMapping> forward_;
    std::vector<std::vector<int>> reverse_;   // reverse_[type][real_fd] = some vfd
    std::vector<std::vector<int>> refcount_;  // refcount_[type][real_fd]
    std::vector<int> free_list_;
    std::size_t watermark_ = 0;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_FD_REMAP_HPP_
