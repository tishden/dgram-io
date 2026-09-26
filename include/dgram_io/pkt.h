// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Ethernet/IPv4/UDP frame construction and parsing for the L2 backends (xdp
// builds into a UMEM frame, dpdk into an rte_mbuf -- the same bytes). Pure
// functions over caller-owned buffers: no sockets, no libxdp, unit-testable
// without root (tests/test_pktbuild.cpp).
//
// Scope is deliberately narrow -- a point-to-point or single-subnet wire:
//  * IPv4 only, no IP options on TX, options rejected on RX (ihl must be 5);
//  * no fragmentation (frames fit the MTU by construction, DF is set);
//  * UDP checksum 0 (legal for IPv4) -- neither XDP nor the NIC verifies it,
//    and the kernel-UDP interop path accepts zero checksums;
//  * IP header checksum is computed properly so a kernel-UDP peer does not
//    drop our frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dgram_io/backend.h"

namespace dgram_io {
namespace pkt {

inline constexpr size_t kEthLen = 14;
inline constexpr size_t kIpLen = 20;
inline constexpr size_t kUdpLen = 8;
inline constexpr size_t kHdrLen = kEthLen + kIpLen + kUdpLen;  // 42

inline constexpr uint16_t kEthIpv4 = 0x0800;  // host order
inline constexpr uint8_t kProtoUdp = 17;
// Minimum Ethernet frame (without FCS). The kernel pads runts on transmit;
// the XDP TX path and a DPDK PMD do not, and the peer NIC may silently drop
// what arrives short -- a datagram of under 18 bytes would simply vanish.
inline constexpr size_t kMinFrame = 60;

// Unconditional 16-bit byte swap: host (little-endian) to network order,
// without <arpa/inet.h>. x86-64 only, like the L2 backends that use it.
inline uint16_t bswap16(uint16_t v) {
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

// RFC 1071 ones'-complement sum over the 20-byte IPv4 header.
inline uint16_t ip_checksum(const uint8_t* hdr, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < len; i += 2)
    sum += static_cast<uint32_t>(hdr[i]) << 8 | hdr[i + 1];
  if (len & 1) sum += static_cast<uint32_t>(hdr[len - 1]) << 8;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  const uint16_t folded = static_cast<uint16_t>(~sum);
  return bswap16(folded);  // network order, ready to memcpy at offset 10
}

// Class-D test (224/4). Byte-wise so this header stays free of arpa/inet.h;
// the companion of mcast_mac -- every backend classifies Config::dst_ip
// with this.
inline bool is_mcast(uint32_t ip_be) {
  return (reinterpret_cast<const uint8_t*>(&ip_be)[0] >> 4) == 0xe;
}

// IPv4 multicast group -> 01:00:5e + low 23 bits of the group address.
inline void mcast_mac(uint32_t group_ip_be, uint8_t mac[6]) {
  const uint8_t* g = reinterpret_cast<const uint8_t*>(&group_ip_be);
  mac[0] = 0x01;
  mac[1] = 0x00;
  mac[2] = 0x5e;
  mac[3] = g[1] & 0x7f;
  mac[4] = g[2];
  mac[5] = g[3];
}

// Everything constant across a flow's frames, precomputed once.
struct Template {
  uint8_t src_mac[6];
  uint8_t dst_mac[6];
  uint32_t src_ip_be;
  uint32_t dst_ip_be;
  uint16_t src_port_be;
  uint16_t dst_port_be;
};

// The queue_to() path of every L2 backend: same flow template, different
// destination (a reply goes to the endpoint learned from RX).
inline Template with_dst(const Template& t, const dgram_io::Endpoint& to) {
  Template r = t;
  std::memcpy(r.dst_mac, to.mac, 6);
  r.dst_ip_be = to.ip_be;
  r.dst_port_be = to.port_be;
  return r;
}

// "aa:bb:cc:dd:ee:ff" -> bytes; shared by the xdp and dpdk backends. Strict:
// each octet is at most two hex digits and the string must end after the
// sixth -- this is the fail-fast validator for Config::dst_mac, and on the
// DPDK path a silently mis-parsed MAC means frames blackhole with no counter.
inline bool parse_mac(const char* s, uint8_t mac[6]) {
  unsigned v[6];
  int end = -1;
  if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%n", &v[0], &v[1], &v[2], &v[3],
             &v[4], &v[5], &end) != 6 ||
      end < 0 || s[end] != '\0')
    return false;
  for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(v[i]);
  return true;
}

// Writes headers + payload into frame (caller guarantees kHdrLen + len bytes);
// returns the frame length. ip_id feeds the IPv4 identification field -- pass
// an incrementing counter (pcap readability; nothing depends on it). len must
// not exceed 65507, the most an IPv4 datagram can carry.
inline size_t build(uint8_t* frame, const Template& t, uint16_t ip_id,
                    const void* payload, size_t len) {
  // Ethernet
  std::memcpy(frame, t.dst_mac, 6);
  std::memcpy(frame + 6, t.src_mac, 6);
  frame[12] = kEthIpv4 >> 8;
  frame[13] = kEthIpv4 & 0xff;
  // IPv4
  uint8_t* ip = frame + kEthLen;
  const uint16_t ip_total = static_cast<uint16_t>(kIpLen + kUdpLen + len);
  ip[0] = 0x45;  // version 4, ihl 5
  ip[1] = 0;     // DSCP/ECN
  ip[2] = ip_total >> 8;
  ip[3] = ip_total & 0xff;
  ip[4] = ip_id >> 8;
  ip[5] = ip_id & 0xff;
  ip[6] = 0x40;  // DF, no fragments
  ip[7] = 0;
  ip[8] = 64;  // TTL
  ip[9] = kProtoUdp;
  ip[10] = ip[11] = 0;  // checksum slot
  std::memcpy(ip + 12, &t.src_ip_be, 4);
  std::memcpy(ip + 16, &t.dst_ip_be, 4);
  const uint16_t csum = ip_checksum(ip, kIpLen);
  std::memcpy(ip + 10, &csum, 2);
  // UDP (checksum 0)
  uint8_t* udp = frame + kEthLen + kIpLen;
  std::memcpy(udp, &t.src_port_be, 2);
  std::memcpy(udp + 2, &t.dst_port_be, 2);
  const uint16_t udp_total = static_cast<uint16_t>(kUdpLen + len);
  udp[4] = udp_total >> 8;
  udp[5] = udp_total & 0xff;
  udp[6] = udp[7] = 0;
  std::memcpy(frame + kHdrLen, payload, len);
  size_t flen = kHdrLen + len;
  if (flen < kMinFrame) {  // pad runts; IP/UDP lengths keep the true payload
    std::memset(frame + flen, 0, kMinFrame - flen);
    flen = kMinFrame;
  }
  return flen;
}

// Parse result: view into the frame + the sender's endpoint (to reply to).
struct View {
  const uint8_t* payload;
  uint32_t len;
  dgram_io::Endpoint from;
  uint16_t dst_port_be;
};

// Accepts exactly what we emit: IPv4 without options, UDP, sane lengths.
// Returns false for anything else (the caller counts and drops).
inline bool parse(const uint8_t* frame, size_t frame_len, View* v) {
  if (frame_len < kHdrLen) return false;
  if (frame[12] != (kEthIpv4 >> 8) || frame[13] != (kEthIpv4 & 0xff))
    return false;
  const uint8_t* ip = frame + kEthLen;
  if (ip[0] != 0x45) return false;  // v4, no options
  if (ip[9] != kProtoUdp) return false;
  if (ip[6] & 0x3f || ip[7]) return false;  // fragment offset / MF
  const size_t ip_total = static_cast<size_t>(ip[2]) << 8 | ip[3];
  if (ip_total < kIpLen + kUdpLen || ip_total > frame_len - kEthLen)
    return false;
  const uint8_t* udp = ip + kIpLen;
  const size_t udp_total = static_cast<size_t>(udp[4]) << 8 | udp[5];
  if (udp_total < kUdpLen || udp_total > ip_total - kIpLen) return false;
  v->payload = frame + kHdrLen;
  v->len = static_cast<uint32_t>(udp_total - kUdpLen);
  std::memcpy(v->from.mac, frame + 6, 6);  // Ethernet source
  std::memcpy(&v->from.ip_be, ip + 12, 4);
  std::memcpy(&v->from.port_be, udp, 2);
  std::memcpy(&v->dst_port_be, udp + 2, 2);
  return true;
}

}  // namespace pkt
}  // namespace dgram_io
