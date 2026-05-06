// Unit tests for the RDMA QpInfo wire codec.

#include "socksdirect/rdma/qp_info.hpp"

#include <gtest/gtest.h>
#include <cstring>

using socksdirect::rdma::QpInfo;
using socksdirect::rdma::encode_qp_info;
using socksdirect::rdma::decode_qp_info;

namespace {

TEST(QpInfo, RoundTrip) {
    QpInfo a{};
    a.qp_num      = 0x12345678;
    a.lid         = 0xabcd;
    for (int i = 0; i < 16; ++i) a.gid[i] = static_cast<std::uint8_t>(i + 1);
    a.rkey        = 0xdeadbeef;
    a.remote_addr = 0x1122334455667788ULL;
    a.mtu         = 5;  // ibv_mtu = 4096

    auto enc = encode_qp_info(a);
    EXPECT_EQ(128u, enc.size());

    QpInfo b{};
    ASSERT_TRUE(decode_qp_info(enc, b));
    EXPECT_EQ(a.qp_num,      b.qp_num);
    EXPECT_EQ(a.lid,         b.lid);
    EXPECT_EQ(0, std::memcmp(a.gid, b.gid, 16));
    EXPECT_EQ(a.rkey,        b.rkey);
    EXPECT_EQ(a.remote_addr, b.remote_addr);
    EXPECT_EQ(a.mtu,         b.mtu);
}

TEST(QpInfo, RejectsWrongLength) {
    QpInfo b{};
    EXPECT_FALSE(decode_qp_info("short", b));
    EXPECT_FALSE(decode_qp_info(std::string(127, 'a'), b));
    EXPECT_FALSE(decode_qp_info(std::string(129, 'a'), b));
}

TEST(QpInfo, RejectsNonHex) {
    QpInfo b{};
    std::string s(128, 'a');
    s[10] = 'z';
    EXPECT_FALSE(decode_qp_info(s, b));
}

TEST(QpInfo, ZeroIsValid) {
    QpInfo a{};
    auto enc = encode_qp_info(a);
    QpInfo b{};
    b.qp_num = 1;  // pre-pollute
    ASSERT_TRUE(decode_qp_info(enc, b));
    EXPECT_EQ(0u, b.qp_num);
}

TEST(QpInfo, AbiSizeIsStable) {
    EXPECT_EQ(64u, sizeof(QpInfo));
}

}  // namespace
