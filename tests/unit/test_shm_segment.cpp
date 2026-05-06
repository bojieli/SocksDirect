// Unit tests for socksdirect::ShmSegment.
//
// We test the segment within a single process (creator + joiner are
// two ShmSegment instances pointing at the same key). The end-to-end
// across-process scenario is exercised by tests/integration/.

#include "socksdirect/shm_segment.hpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <random>
#include <thread>

namespace {

std::uint64_t fresh_key() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng() | 1;  // never 0
}

class SegmentFixture : public ::testing::Test {
protected:
    std::uint64_t key = 0;
    socksdirect::ShmSegment creator;
    socksdirect::ShmSegment joiner;

    void SetUp() override {
        key = fresh_key();
    }
    void TearDown() override {
        if (creator.is_open()) creator.close();
        if (joiner.is_open())  joiner.close();
        socksdirect::ShmSegment::unlink_by_key(key);
    }
};

TEST_F(SegmentFixture, OpenCreatorThenJoinerSucceeds) {
    ASSERT_EQ(0, creator.open(key, socksdirect::ShmSegment::kRoleCreator)) << strerror(errno);
    ASSERT_TRUE(creator.is_open());
    ASSERT_EQ(0, joiner.open(key, socksdirect::ShmSegment::kRoleJoiner)) << strerror(errno);
    ASSERT_TRUE(joiner.is_open());
    EXPECT_EQ(2, creator.header()->refcount.load());
    EXPECT_EQ(socksdirect::kShmSegmentMagic, creator.header()->magic.load());
    EXPECT_NE(0, creator.header()->creator_pid);
    EXPECT_NE(0, creator.header()->joiner_pid);
}

TEST_F(SegmentFixture, JoinerWithoutCreatorFails) {
    ASSERT_EQ(-1, joiner.open(key, socksdirect::ShmSegment::kRoleJoiner));
    EXPECT_EQ(ENOENT, errno);
}

TEST_F(SegmentFixture, RingsAreCrossWired) {
    ASSERT_EQ(0, creator.open(key, socksdirect::ShmSegment::kRoleCreator));
    ASSERT_EQ(0, joiner.open(key, socksdirect::ShmSegment::kRoleJoiner));
    // Each ShmSegment instance is a separate mmap of the same backing
    // file; the in-process pointers differ, but the underlying bytes
    // are shared. Verify the cross-wiring by writing on one side and
    // reading on the other (in both directions).
    char buf[8];

    // creator -> joiner.
    EXPECT_EQ(4u, creator.ring_outbound()->send_some("ABCD", 4));
    EXPECT_EQ(4u, joiner.ring_inbound()->recv_some(buf, 4));
    EXPECT_EQ(0, std::memcmp(buf, "ABCD", 4));

    // joiner -> creator.
    EXPECT_EQ(4u, joiner.ring_outbound()->send_some("WXYZ", 4));
    EXPECT_EQ(4u, creator.ring_inbound()->recv_some(buf, 4));
    EXPECT_EQ(0, std::memcmp(buf, "WXYZ", 4));
}

TEST_F(SegmentFixture, ByteStreamRoundTrip) {
    ASSERT_EQ(0, creator.open(key, socksdirect::ShmSegment::kRoleCreator));
    ASSERT_EQ(0, joiner.open(key, socksdirect::ShmSegment::kRoleJoiner));

    const char msg[] = "hello shm";
    EXPECT_EQ(sizeof(msg),
              creator.ring_outbound()->send_some(msg, sizeof(msg)));
    char buf[32] = {0};
    EXPECT_EQ(sizeof(msg),
              joiner.ring_inbound()->recv_some(buf, sizeof(buf)));
    EXPECT_STREQ(msg, buf);

    const char rep[] = "ack";
    EXPECT_EQ(sizeof(rep),
              joiner.ring_outbound()->send_some(rep, sizeof(rep)));
    EXPECT_EQ(sizeof(rep),
              creator.ring_inbound()->recv_some(buf, sizeof(buf)));
    EXPECT_STREQ(rep, buf);
}

TEST_F(SegmentFixture, RefcountFallsToZeroAndUnlinks) {
    ASSERT_EQ(0, creator.open(key, socksdirect::ShmSegment::kRoleCreator));
    ASSERT_EQ(0, joiner.open(key, socksdirect::ShmSegment::kRoleJoiner));
    EXPECT_EQ(2, creator.header()->refcount.load());
    creator.close();
    EXPECT_FALSE(creator.is_open());
    // After creator closes, joiner sees refcount=1 and the segment's
    // outbound ring is marked closed for the joiner-side recv.
    EXPECT_TRUE(joiner.ring_inbound()->peer_closed());
    EXPECT_EQ(1, joiner.header()->refcount.load());
    joiner.close();
    // Now /dev/shm/sd-<key> should be gone.
    socksdirect::ShmSegment again;
    EXPECT_EQ(-1, again.open(key, socksdirect::ShmSegment::kRoleJoiner));
    EXPECT_EQ(ENOENT, errno);
}

TEST_F(SegmentFixture, CreatorSurvivesStaleSegmentOnDisk) {
    // Fake a leftover segment on disk; the creator should clobber it.
    std::string name = socksdirect::ShmSegment::name_for_key(key);
    int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT, 0660);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::ftruncate(fd, 4096));
    ::close(fd);
    ASSERT_EQ(0, creator.open(key, socksdirect::ShmSegment::kRoleCreator));
    creator.close();
}

TEST_F(SegmentFixture, KeyNamingIsStableHex) {
    EXPECT_EQ("/sd-0000000000000001",
              socksdirect::ShmSegment::name_for_key(1));
    EXPECT_EQ("/sd-deadbeefcafebabe",
              socksdirect::ShmSegment::name_for_key(0xdeadbeefcafebabeULL));
}

TEST_F(SegmentFixture, LayoutBytesIsPageRounded) {
    // Header (64) + 2 * ring (1 MiB each + their own headers) -> ~2 MiB.
    auto sz = socksdirect::ShmSegment::layout_bytes();
    EXPECT_EQ(0u, sz % 4096);
    EXPECT_GE(sz, 2u * (1u << 20));
}

}  // namespace
