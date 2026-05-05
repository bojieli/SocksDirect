// Unit tests for socksdirect::ZeroCopyClient.
//
// We can't load a real kernel module from a unit test. Instead we test:
//   - Open against an absent path -> kCopyFallback, ops return ENOSYS.
//   - Default-constructed client is in copy fallback.
//   - The struct definitions in zerocopy.h satisfy the layout/size
//     constraints the LKM relies on.
//   - The ioctl request numbers are stable. (Encoded into a simple
//     fingerprint to catch accidental ABI breakage.)

#include "socksdirect/zerocopy_client.hpp"

#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>

namespace {

TEST(ZeroCopyClient, DefaultIsCopyFallback) {
    socksdirect::ZeroCopyClient zc;
    EXPECT_EQ(socksdirect::ZeroCopyClient::kCopyFallback, zc.status());
    EXPECT_EQ(-1, zc.device_fd());

    // Operations must fail with ENOSYS so callers fall back to memcpy.
    uint64_t addr = 0, cookie = 0;
    EXPECT_EQ(-1, zc.alloc_phys(1, &addr, &cookie));
    EXPECT_EQ(ENOSYS, zc.last_errno());

    EXPECT_EQ(-1, zc.virt2phys(0xdeadbeef, &cookie));
    EXPECT_EQ(ENOSYS, zc.last_errno());

    EXPECT_EQ(-1, zc.echo(123, &cookie));
    EXPECT_EQ(ENOSYS, zc.last_errno());
}

TEST(ZeroCopyClient, OpenMissingDeviceLeavesCopyFallback) {
    socksdirect::ZeroCopyClient zc;
    auto st = zc.open("/dev/socksdirect-not-real-nope");
    EXPECT_EQ(socksdirect::ZeroCopyClient::kCopyFallback, st);
    EXPECT_EQ(socksdirect::ZeroCopyClient::kCopyFallback, zc.status());
    EXPECT_NE(0, zc.last_errno());
}

TEST(ZeroCopyClient, MoveTransfersOwnership) {
    socksdirect::ZeroCopyClient a;
    socksdirect::ZeroCopyClient b(std::move(a));
    // a is now defaulted; b inherits the absent state.
    EXPECT_EQ(socksdirect::ZeroCopyClient::kCopyFallback, a.status());
    EXPECT_EQ(socksdirect::ZeroCopyClient::kCopyFallback, b.status());
}

// --- Layout / ABI tests ---

TEST(ZeroCopyAbi, StructSizesAreStable) {
    // These sizes are baked into the LKM. Bumping any of them is an ABI
    // break and requires a major-version bump.
    EXPECT_EQ(8u,  sizeof(sd_version));
    EXPECT_EQ(24u, sizeof(sd_alloc_phys));
    EXPECT_EQ(8u,  sizeof(sd_free_phys));
    EXPECT_EQ(16u, sizeof(sd_virt2phys));
    EXPECT_EQ(24u, sizeof(sd_virt2phys_vec));
    EXPECT_EQ(24u, sizeof(sd_map_phys));
    EXPECT_EQ(32u, sizeof(sd_map_phys_vec));
    EXPECT_EQ(16u, sizeof(sd_echo));
}

TEST(ZeroCopyAbi, IoctlNumbersAreStable) {
    // Same rationale: the kernel module pattern-matches on these. Any
    // change here is a wire-protocol break.
    EXPECT_EQ(SD_IOC_GET_VERSION,    _IOR(0xCD, 0x01, sd_version));
    EXPECT_EQ(SD_IOC_ALLOC_PHYS,     _IOWR(0xCD, 0x02, sd_alloc_phys));
    EXPECT_EQ(SD_IOC_FREE_PHYS,      _IOW(0xCD, 0x03, sd_free_phys));
    EXPECT_EQ(SD_IOC_VIRT2PHYS,      _IOWR(0xCD, 0x04, sd_virt2phys));
    EXPECT_EQ(SD_IOC_VIRT2PHYS_VEC,  _IOWR(0xCD, 0x05, sd_virt2phys_vec));
    EXPECT_EQ(SD_IOC_MAP_PHYS,       _IOWR(0xCD, 0x06, sd_map_phys));
    EXPECT_EQ(SD_IOC_MAP_PHYS_VEC,   _IOWR(0xCD, 0x07, sd_map_phys_vec));
    EXPECT_EQ(SD_IOC_ECHO,           _IOWR(0xCD, 0x08, sd_echo));
}

TEST(ZeroCopyAbi, MajorMinorIs1_0) {
    EXPECT_EQ(1, SOCKSDIRECT_ABI_MAJOR);
    EXPECT_EQ(0, SOCKSDIRECT_ABI_MINOR);
}

}  // namespace
