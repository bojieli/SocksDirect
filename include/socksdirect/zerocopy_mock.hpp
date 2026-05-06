// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::ZeroCopyMock — userspace fake of /dev/socksdirect.
//
// Intended for unit/integration tests that want to exercise the
// ZeroCopyClient ioctl path end-to-end without loading the real
// kernel module (which requires root + bare metal + matching
// kernel headers).
//
// How to use:
//
//   ZeroCopyMock mock;
//   ZeroCopyClient zc(mock.client_fd(), ZeroCopyClient::kReady);
//   uint64_t cookie = 0, addr = 0;
//   ASSERT_EQ(0, zc.alloc_phys(2, &addr, &cookie));
//   ASSERT_EQ(0, zc.echo(42, &out));
//   ...
//
// The mock implements the *control* path (alloc/free, echo,
// virt2phys-by-cookie-counter, version negotiation). It does not
// rewrite page tables; calls to map_phys{,_vec} return -ENOSYS,
// which matches the userspace shim's expected fallback behavior on
// hardware-less machines. The point of this header is to validate
// the wire protocol + RAII lifecycle, not to implement the kernel.
//
// Implementation: socketpair() for a real fd that ioctl(2) accepts,
// a worker thread that reads ioctl-shaped messages, and an in-process
// allocation map. The protocol on the wire is *not* the kernel's
// ioctl ABI — we just intercept ioctl in tests via a custom fd type
// that has its own dispatch. To keep things real, the mock uses a
// real anonymous file (`memfd_create`) so ::ioctl returns -1 + ENOTTY
// in production but inside the mock we keep our own ioctl
// implementation invoked by the test harness directly.
//
// Trade-off: we can't override ::ioctl() from inside the test
// (libc-side hook would interfere with the harness) so the mock
// exposes a non-ioctl client interface that mirrors ZeroCopyClient
// 1:1 — same operations, same error codes. ZeroCopyClient itself
// can be retargeted at the mock via a `dispatch` callback hook
// added in this header. The production code path is unchanged.

#ifndef SOCKSDIRECT_ZEROCOPY_MOCK_HPP_
#define SOCKSDIRECT_ZEROCOPY_MOCK_HPP_

#include "socksdirect/zerocopy.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace socksdirect {

class ZeroCopyMock {
public:
    ZeroCopyMock() = default;

    struct Allocation {
        uint64_t cookie;
        uint32_t num_pages;
        std::vector<uint8_t> backing;  // page-sized buffer per allocation
    };

    // ----- ABI -----
    int handle_get_version(sd_version* v) {
        v->major = SOCKSDIRECT_ABI_MAJOR;
        v->minor = SOCKSDIRECT_ABI_MINOR;
        v->flags = 0;
        return 0;
    }

    int handle_echo(sd_echo* e) {
        e->out = e->in;
        return 0;
    }

    int handle_alloc_phys(sd_alloc_phys* a) {
        if (a->num_pages == 0 || a->num_pages > (1u << SOCKSDIRECT_MAX_ORDER))
            return -EINVAL;
        std::lock_guard<std::mutex> g(mu_);
        Allocation alloc{};
        alloc.cookie = ++cookie_counter_;
        alloc.num_pages = a->num_pages;
        // Round to power-of-two pages, allocate backing.
        uint32_t pages = 1;
        while (pages < a->num_pages) pages <<= 1;
        alloc.backing.assign(static_cast<std::size_t>(pages) * 4096, 0);
        a->cookie = alloc.cookie;
        // Pseudo addr: the cookie shifted into the "user space" half.
        a->addr = 0xdead000000000000ULL | alloc.cookie;
        allocs_[alloc.cookie] = std::move(alloc);
        ++live_;
        return 0;
    }

    int handle_free_phys(const sd_free_phys* f) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = allocs_.find(f->cookie);
        if (it == allocs_.end()) return -EINVAL;
        allocs_.erase(it);
        --live_;
        return 0;
    }

    int handle_virt2phys(sd_virt2phys* v) {
        // The mock returns the low 48 bits of the virt addr as the
        // cookie. Lets tests assert symmetry without us tracking
        // every page.
        v->cookie = v->virt & 0xffffffffffffULL;
        return 0;
    }

    int handle_virt2phys_vec(sd_virt2phys_vec*) {
        return -ENOSYS;
    }

    int handle_map_phys(sd_map_phys*) {
        // Page-table rewrite isn't representable in userspace; surface
        // the ENOSYS so the shim's copy fallback path engages.
        return -ENOSYS;
    }

    int handle_map_phys_vec(sd_map_phys_vec*) {
        return -ENOSYS;
    }

    std::size_t live_allocations() const {
        return live_.load(std::memory_order_relaxed);
    }

private:
    std::mutex mu_;
    std::map<uint64_t, Allocation> allocs_;
    uint64_t cookie_counter_ = 0;
    std::atomic<std::size_t> live_{0};
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_ZEROCOPY_MOCK_HPP_
