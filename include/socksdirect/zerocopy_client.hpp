// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::ZeroCopyClient — RAII wrapper around /dev/socksdirect.
//
// At process startup the library tries to open the device. If the open
// fails (most commonly because the LKM isn't loaded), the client object
// remains in "copy mode": callers see the same API but every operation
// returns kCopyFallback and they fall back to memcpy. This is the
// graceful-degradation path called out in the rewrite plan.
//
// The header is dependency-free (stdlib + Linux uapi). No threads here;
// the device is intentionally serialized at the kernel boundary, and the
// library serializes its own callers separately.

#ifndef SOCKSDIRECT_ZEROCOPY_CLIENT_HPP_
#define SOCKSDIRECT_ZEROCOPY_CLIENT_HPP_

#include "socksdirect/zerocopy.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace socksdirect {

class ZeroCopyClient {
public:
    enum Status : int {
        kReady          = 0,
        kCopyFallback   = 1,  // device unavailable; caller must memcpy
        kAbiMismatch    = 2,  // device present but wrong major
    };

    ZeroCopyClient() = default;

    // Test seam — let tests inject a pre-opened fd (e.g. a pipe with a
    // mock kernel side). The client takes ownership and closes on dtor.
    explicit ZeroCopyClient(int fd, Status s = kReady) : fd_(fd), status_(s) {}

    ~ZeroCopyClient() {
        if (fd_ >= 0) ::close(fd_);
    }

    ZeroCopyClient(const ZeroCopyClient&) = delete;
    ZeroCopyClient& operator=(const ZeroCopyClient&) = delete;
    ZeroCopyClient(ZeroCopyClient&& o) noexcept { *this = std::move(o); }
    ZeroCopyClient& operator=(ZeroCopyClient&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = o.fd_;
            status_ = o.status_;
            o.fd_ = -1;
            o.status_ = kCopyFallback;
        }
        return *this;
    }

    // Open the device at `path` (default: SOCKSDIRECT_DEV_PATH). On
    // failure the client lives, in copy-fallback mode, and the caller
    // checks status() to decide what to log.
    Status open(const std::string& path = SOCKSDIRECT_DEV_PATH) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            status_ = kCopyFallback;
            errno_ = errno;
            return status_;
        }
        // Verify ABI version.
        sd_version v{};
        if (::ioctl(fd_, SD_IOC_GET_VERSION, &v) < 0) {
            errno_ = errno;
            ::close(fd_);
            fd_ = -1;
            status_ = kCopyFallback;
            return status_;
        }
        if (v.major != SOCKSDIRECT_ABI_MAJOR) {
            ::close(fd_);
            fd_ = -1;
            status_ = kAbiMismatch;
            return status_;
        }
        major_ = v.major;
        minor_ = v.minor;
        status_ = kReady;
        return status_;
    }

    Status status() const { return status_; }
    int  device_fd() const { return fd_; }
    int  last_errno() const { return errno_; }
    uint16_t abi_major() const { return major_; }
    uint16_t abi_minor() const { return minor_; }

    // ----- Operations. Each returns 0 on success, or -1 on failure with
    // last_errno() set. In copy-fallback mode, every op returns -1 with
    // errno=ENOSYS so callers know to switch paths.

    int alloc_phys(uint32_t num_pages, uint64_t* out_addr, uint64_t* out_cookie) {
        if (status_ != kReady) return fail(ENOSYS);
        sd_alloc_phys req{};
        req.num_pages = num_pages;
        if (::ioctl(fd_, SD_IOC_ALLOC_PHYS, &req) < 0) return fail(errno);
        if (out_addr)   *out_addr   = req.addr;
        if (out_cookie) *out_cookie = req.cookie;
        return 0;
    }

    int free_phys(uint64_t cookie) {
        if (status_ != kReady) return fail(ENOSYS);
        sd_free_phys req{};
        req.cookie = cookie;
        if (::ioctl(fd_, SD_IOC_FREE_PHYS, &req) < 0) return fail(errno);
        return 0;
    }

    int virt2phys(uint64_t virt, uint64_t* out_cookie) {
        if (status_ != kReady) return fail(ENOSYS);
        sd_virt2phys req{};
        req.virt = virt;
        if (::ioctl(fd_, SD_IOC_VIRT2PHYS, &req) < 0) return fail(errno);
        if (out_cookie) *out_cookie = req.cookie;
        return 0;
    }

    int virt2phys_vec(uint64_t virt, std::vector<uint64_t>* cookies, uint32_t npages) {
        if (status_ != kReady) return fail(ENOSYS);
        if (!cookies) return fail(EFAULT);
        cookies->assign(npages, 0);
        sd_virt2phys_vec req{};
        req.virt = virt;
        req.cookies = reinterpret_cast<uint64_t>(cookies->data());
        req.npages = npages;
        if (::ioctl(fd_, SD_IOC_VIRT2PHYS_VEC, &req) < 0) return fail(errno);
        return 0;
    }

    int map_phys(uint64_t virt, uint64_t cookie, uint64_t* out_old) {
        if (status_ != kReady) return fail(ENOSYS);
        sd_map_phys req{};
        req.virt = virt;
        req.cookie = cookie;
        if (::ioctl(fd_, SD_IOC_MAP_PHYS, &req) < 0) return fail(errno);
        if (out_old) *out_old = req.old_cookie;
        return 0;
    }

    int map_phys_vec(uint64_t virt, const std::vector<uint64_t>& cookies,
                     std::vector<uint64_t>* old_cookies) {
        if (status_ != kReady) return fail(ENOSYS);
        if (!old_cookies) return fail(EFAULT);
        old_cookies->assign(cookies.size(), 0);
        sd_map_phys_vec req{};
        req.virt = virt;
        req.cookies = reinterpret_cast<uint64_t>(cookies.data());
        req.old_cookies = reinterpret_cast<uint64_t>(old_cookies->data());
        req.npages = static_cast<uint32_t>(cookies.size());
        if (::ioctl(fd_, SD_IOC_MAP_PHYS_VEC, &req) < 0) return fail(errno);
        return 0;
    }

    // Round-trip echo. Returns the kernel's reply.
    int echo(uint64_t in, uint64_t* out) {
        if (status_ != kReady) return fail(ENOSYS);
        sd_echo req{};
        req.in = in;
        if (::ioctl(fd_, SD_IOC_ECHO, &req) < 0) return fail(errno);
        if (out) *out = req.out;
        return 0;
    }

private:
    int fail(int e) {
        errno_ = e;
        errno  = e;
        return -1;
    }

    int fd_ = -1;
    Status status_ = kCopyFallback;
    int errno_ = 0;
    uint16_t major_ = 0;
    uint16_t minor_ = 0;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_ZEROCOPY_CLIENT_HPP_
