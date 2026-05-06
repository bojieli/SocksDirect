// Unit tests for socksdirect::ZeroCopyMock.
//
// The mock lets us validate the userspace-side ABI handling
// (struct-pack semantics, refcount on the alloc/free pair, error
// codes for the unimplemented ops) without loading the kernel
// module.

#include "socksdirect/zerocopy_mock.hpp"

#include <gtest/gtest.h>
#include <cerrno>
#include <thread>
#include <vector>

namespace {

TEST(ZeroCopyMock, GetVersionMatchesAbi) {
    socksdirect::ZeroCopyMock m;
    sd_version v{};
    EXPECT_EQ(0, m.handle_get_version(&v));
    EXPECT_EQ(SOCKSDIRECT_ABI_MAJOR, v.major);
    EXPECT_EQ(SOCKSDIRECT_ABI_MINOR, v.minor);
    EXPECT_EQ(0u, v.flags);
}

TEST(ZeroCopyMock, EchoIsIdentity) {
    socksdirect::ZeroCopyMock m;
    sd_echo e{};
    e.in = 0xdeadbeefcafebabeULL;
    EXPECT_EQ(0, m.handle_echo(&e));
    EXPECT_EQ(e.in, e.out);
}

TEST(ZeroCopyMock, AllocFreeRoundtrip) {
    socksdirect::ZeroCopyMock m;
    sd_alloc_phys a{};
    a.num_pages = 4;
    EXPECT_EQ(0, m.handle_alloc_phys(&a));
    EXPECT_NE(0u, a.cookie);
    EXPECT_NE(0u, a.addr);
    EXPECT_EQ(1u, m.live_allocations());

    sd_free_phys f{};
    f.cookie = a.cookie;
    EXPECT_EQ(0, m.handle_free_phys(&f));
    EXPECT_EQ(0u, m.live_allocations());

    // Double free returns -EINVAL.
    EXPECT_EQ(-EINVAL, m.handle_free_phys(&f));
}

TEST(ZeroCopyMock, AllocRejectsZeroAndOversize) {
    socksdirect::ZeroCopyMock m;
    sd_alloc_phys a{};
    a.num_pages = 0;
    EXPECT_EQ(-EINVAL, m.handle_alloc_phys(&a));
    a.num_pages = (1u << SOCKSDIRECT_MAX_ORDER) + 1;
    EXPECT_EQ(-EINVAL, m.handle_alloc_phys(&a));
}

TEST(ZeroCopyMock, ConcurrentAllocFreeIsSafe) {
    socksdirect::ZeroCopyMock m;
    constexpr int kThreads = 8;
    constexpr int kPer = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]() {
            for (int i = 0; i < kPer; ++i) {
                sd_alloc_phys a{}; a.num_pages = 1;
                ASSERT_EQ(0, m.handle_alloc_phys(&a));
                sd_free_phys f{}; f.cookie = a.cookie;
                ASSERT_EQ(0, m.handle_free_phys(&f));
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(0u, m.live_allocations());
}

TEST(ZeroCopyMock, MapPhysFamilyReturnsEnosys) {
    socksdirect::ZeroCopyMock m;
    sd_map_phys mp{};
    EXPECT_EQ(-ENOSYS, m.handle_map_phys(&mp));
    sd_map_phys_vec mpv{};
    EXPECT_EQ(-ENOSYS, m.handle_map_phys_vec(&mpv));
    sd_virt2phys_vec v2pv{};
    EXPECT_EQ(-ENOSYS, m.handle_virt2phys_vec(&v2pv));
}

TEST(ZeroCopyMock, Virt2PhysIsLowBitPassthrough) {
    socksdirect::ZeroCopyMock m;
    sd_virt2phys v{};
    v.virt = 0xffff800000123000ULL;
    EXPECT_EQ(0, m.handle_virt2phys(&v));
    EXPECT_EQ(0x800000123000ULL, v.cookie);
}

}  // namespace
