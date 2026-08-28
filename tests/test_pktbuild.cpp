// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Unit tests for io/pkt.h: Ethernet/IPv4/UDP frame construction and parsing
// used by the L2 backends (AF_XDP now, DPDK later). No root, no sockets.
#include "dgram_io/pkt.h"

using namespace dgram_io;  // NOLINT: test-local convenience

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      return 1;                                                        \
    }                                                                  \
  } while (0)

namespace {

pkt::Template make_template() {
  pkt::Template t{};
  const uint8_t src[6] = {0x90, 0xe2, 0xba, 0xed, 0x3d, 0x34};
  const uint8_t dst[6] = {0x90, 0xe2, 0xba, 0xed, 0x3d, 0x35};
  std::memcpy(t.src_mac, src, 6);
  std::memcpy(t.dst_mac, dst, 6);
  inet_pton(AF_INET, "192.168.77.1", &t.src_ip_be);
  inet_pton(AF_INET, "192.168.77.2", &t.dst_ip_be);
  t.src_port_be = htons(5701);
  t.dst_port_be = htons(5701);
  return t;
}

// Independent reference: RFC 1071 checksum via 16-bit words, verifying that
// checksumming a header with its checksum field in place yields 0xffff.
uint16_t ref_sum_with_checksum(const uint8_t* ip) {
  uint32_t sum = 0;
  for (int i = 0; i < 20; i += 2)
    sum += static_cast<uint32_t>(ip[i]) << 8 | ip[i + 1];
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(sum);
}

}  // namespace

int main() {
  const pkt::Template t = make_template();
  uint8_t payload[64];
  for (size_t i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(i * 7);

  // Build: layout and field values.
  uint8_t frame[pkt::kHdrLen + sizeof(payload)];
  const size_t flen = pkt::build(frame, t, 0x1234, payload, sizeof(payload));
  CHECK(flen == pkt::kHdrLen + sizeof(payload));
  CHECK(std::memcmp(frame, t.dst_mac, 6) == 0);
  CHECK(std::memcmp(frame + 6, t.src_mac, 6) == 0);
  CHECK(frame[12] == 0x08 && frame[13] == 0x00);  // IPv4 ethertype
  const uint8_t* ip = frame + pkt::kEthLen;
  CHECK(ip[0] == 0x45);
  CHECK((static_cast<size_t>(ip[2]) << 8 | ip[3]) ==
        pkt::kIpLen + pkt::kUdpLen + sizeof(payload));
  CHECK(ip[4] == 0x12 && ip[5] == 0x34);  // ip_id
  CHECK(ip[6] == 0x40 && ip[7] == 0);     // DF, offset 0
  CHECK(ip[9] == pkt::kProtoUdp);
  CHECK(std::memcmp(ip + 12, &t.src_ip_be, 4) == 0);
  CHECK(std::memcmp(ip + 16, &t.dst_ip_be, 4) == 0);
  // Checksum verifies against the reference: sum over the full header
  // including the checksum field must be 0xffff.
  CHECK(ref_sum_with_checksum(ip) == 0xffff);
  const uint8_t* udp = ip + pkt::kIpLen;
  CHECK(std::memcmp(udp, &t.src_port_be, 2) == 0);
  CHECK(std::memcmp(udp + 2, &t.dst_port_be, 2) == 0);
  CHECK((static_cast<size_t>(udp[4]) << 8 | udp[5]) ==
        pkt::kUdpLen + sizeof(payload));
  CHECK(udp[6] == 0 && udp[7] == 0);  // UDP checksum 0 (legal for IPv4)

  // Parse: roundtrip and endpoint extraction.
  pkt::View v{};
  CHECK(pkt::parse(frame, flen, &v));
  CHECK(v.len == sizeof(payload));
  CHECK(std::memcmp(v.payload, payload, sizeof(payload)) == 0);
  CHECK(std::memcmp(v.from.mac, t.src_mac, 6) == 0);
  CHECK(v.from.ip_be == t.src_ip_be);
  CHECK(v.from.port_be == t.src_port_be);
  CHECK(v.dst_port_be == t.dst_port_be);

  // Padded frame (Ethernet min-frame padding): UDP length wins over frame_len.
  uint8_t padded[pkt::kHdrLen + sizeof(payload) + 18];
  std::memset(padded, 0, sizeof(padded));
  std::memcpy(padded, frame, flen);
  CHECK(pkt::parse(padded, sizeof(padded), &v));
  CHECK(v.len == sizeof(payload));

  // Small payloads pad up to the 60-byte Ethernet minimum (zero-copy XDP TX
  // does not pad runts and the peer NIC drops them); the UDP length keeps the
  // true payload size, so parse returns it unchanged.
  uint8_t hb[pkt::kMinFrame];
  CHECK(pkt::build(hb, t, 1, payload, 0) == pkt::kMinFrame);
  CHECK(pkt::parse(hb, sizeof(hb), &v));
  CHECK(v.len == 0);
  uint8_t small[pkt::kMinFrame];
  CHECK(pkt::build(small, t, 1, payload, 16) == pkt::kMinFrame);  // 58 -> 60
  CHECK(pkt::parse(small, sizeof(small), &v));
  CHECK(v.len == 16);
  CHECK(std::memcmp(v.payload, payload, 16) == 0);

  // Rejections.
  CHECK(!pkt::parse(frame, pkt::kHdrLen - 1, &v));  // truncated
  uint8_t bad[sizeof(frame)];
  std::memcpy(bad, frame, flen);
  bad[12] = 0x86; bad[13] = 0xdd;  // IPv6 ethertype
  CHECK(!pkt::parse(bad, flen, &v));
  std::memcpy(bad, frame, flen);
  bad[pkt::kEthLen] = 0x46;  // IP options (ihl 6)
  CHECK(!pkt::parse(bad, flen, &v));
  std::memcpy(bad, frame, flen);
  bad[pkt::kEthLen + 9] = 6;  // TCP
  CHECK(!pkt::parse(bad, flen, &v));
  std::memcpy(bad, frame, flen);
  bad[pkt::kEthLen + 6] = 0x20;  // fragment offset != 0
  CHECK(!pkt::parse(bad, flen, &v));
  std::memcpy(bad, frame, flen);
  bad[pkt::kEthLen + 2] = 0xff; bad[pkt::kEthLen + 3] = 0xff;  // ip_total > frame
  CHECK(!pkt::parse(bad, flen, &v));
  std::memcpy(bad, frame, flen);
  bad[pkt::kEthLen + pkt::kIpLen + 4] = 0xff;  // udp_total > ip payload
  CHECK(!pkt::parse(bad, flen, &v));

  // Multicast MAC derivation: 239.77.0.1 -> 01:00:5e:4d:00:01 (low 23 bits).
  uint32_t group;
  inet_pton(AF_INET, "239.77.0.1", &group);
  uint8_t mmac[6];
  pkt::mcast_mac(group, mmac);
  const uint8_t want[6] = {0x01, 0x00, 0x5e, 0x4d, 0x00, 0x01};
  CHECK(std::memcmp(mmac, want, 6) == 0);
  // High bit of the second octet is masked: 239.205.0.1 -> same 4d.
  inet_pton(AF_INET, "239.205.0.1", &group);
  pkt::mcast_mac(group, mmac);
  CHECK(mmac[3] == 0x4d);

  // parse_mac: strict -- exactly six 2-hex-digit octets, nothing after.
  uint8_t m[6];
  CHECK(pkt::parse_mac("90:e2:ba:ed:3d:34", m));
  CHECK(m[0] == 0x90 && m[5] == 0x34);
  CHECK(pkt::parse_mac("0:1:2:3:4:5", m));  // single digits are fine
  CHECK(!pkt::parse_mac("90:e2:ba:ed:3d", m));       // five octets
  CHECK(!pkt::parse_mac("90:e2:ba:ed:3d:34:56", m)); // trailing octet
  CHECK(!pkt::parse_mac("90:e2:ba:ed:3d:1ff", m));   // octet > 0xff
  CHECK(!pkt::parse_mac("90:e2:ba:ed:3d:34x", m));   // trailing garbage
  CHECK(!pkt::parse_mac("", m));
  CHECK(!pkt::parse_mac("hello", m));

  // with_dst: overrides exactly the destination triple, keeps the source.
  dgram_io::Endpoint ep{};
  const uint8_t emac[6] = {0x02, 0xaa, 0, 0, 0, 7};
  std::memcpy(ep.mac, emac, 6);
  inet_pton(AF_INET, "10.0.0.7", &ep.ip_be);
  ep.port_be = htons(7777);
  const pkt::Template t3 = pkt::with_dst(t, ep);
  CHECK(std::memcmp(t3.dst_mac, emac, 6) == 0);
  CHECK(t3.dst_ip_be == ep.ip_be && t3.dst_port_be == ep.port_be);
  CHECK(std::memcmp(t3.src_mac, t.src_mac, 6) == 0);
  CHECK(t3.src_ip_be == t.src_ip_be && t3.src_port_be == t.src_port_be);

  // is_mcast: 224/4 exactly.
  uint32_t ipbe;
  inet_pton(AF_INET, "239.77.0.1", &ipbe);
  CHECK(pkt::is_mcast(ipbe));
  inet_pton(AF_INET, "224.0.0.1", &ipbe);
  CHECK(pkt::is_mcast(ipbe));
  inet_pton(AF_INET, "223.255.255.255", &ipbe);
  CHECK(!pkt::is_mcast(ipbe));
  inet_pton(AF_INET, "240.0.0.1", &ipbe);
  CHECK(!pkt::is_mcast(ipbe));
  inet_pton(AF_INET, "192.168.77.2", &ipbe);
  CHECK(!pkt::is_mcast(ipbe));

  // The checksum survives odd header contents (carry folding): all-0xff IPs.
  pkt::Template t2 = t;
  t2.src_ip_be = 0xffffffff;
  t2.dst_ip_be = 0xffffffff;
  CHECK(pkt::build(frame, t2, 0xffff, payload, sizeof(payload)) == flen);
  CHECK(ref_sum_with_checksum(frame + pkt::kEthLen) == 0xffff);

  printf("test_pktbuild: all ok\n");
  return 0;
}
