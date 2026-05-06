// SPDX-License-Identifier: Apache-2.0
//
// Wire-format for the RDMA QP info exchanged via the monitor at
// connection setup time. Both peers register their QP details with
// the monitor's `rdma-register` op; the monitor returns the peer's
// info so each side can transition its QP from INIT to RTR/RTS.
//
// The struct is plain-old-data so it can be encoded as a hex string
// in the NDJSON ctl protocol.

#ifndef SOCKSDIRECT_RDMA_QP_INFO_HPP_
#define SOCKSDIRECT_RDMA_QP_INFO_HPP_

#include <cstdint>
#include <cstring>
#include <string>

namespace socksdirect {
namespace rdma {

struct QpInfo {
    uint32_t qp_num;            // ibv_qp::qp_num                       4
    uint16_t lid;               // local id (IB); 0 on RoCE             2
    uint16_t _pad0;             // align gid to 4                       2
    uint8_t  gid[16];           // RoCEv2 GID (IPv6-formatted IPv4)    16
    uint32_t rkey;              // remote key for the receive buffer    4
    uint32_t _pad1;             // align remote_addr to 8               4
    uint64_t remote_addr;       // remote receive-buffer va             8
    uint32_t mtu;               // ibv_mtu enum value                   4
    uint8_t  reserved[20];      // pad to 64 bytes; zero on the wire   20
} __attribute__((packed));

static_assert(sizeof(QpInfo) == 64, "QpInfo wire size is part of the ABI");

// Encode 64 bytes as 128 hex chars, no separators. Decoder is the
// inverse; both are pure functions, no allocations beyond the result.
inline std::string encode_qp_info(const QpInfo& q) {
    static const char hex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&q);
    std::string out;
    out.resize(sizeof(QpInfo) * 2);
    for (std::size_t i = 0; i < sizeof(QpInfo); ++i) {
        out[2 * i]     = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 0xf];
    }
    return out;
}

inline bool decode_qp_info(const std::string& s, QpInfo& out) {
    if (s.size() != sizeof(QpInfo) * 2) return false;
    auto* bytes = reinterpret_cast<std::uint8_t*>(&out);
    auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < sizeof(QpInfo); ++i) {
        int hi = from_hex(s[2 * i]);
        int lo = from_hex(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace rdma
}  // namespace socksdirect

#endif  // SOCKSDIRECT_RDMA_QP_INFO_HPP_
