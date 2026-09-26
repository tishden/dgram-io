// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// The kernel-UDP socket both socket-datapath backends sit on (udp, uring).
// Shared on purpose: an A/B between sendmmsg/recvmmsg and io_uring should
// compare how datagrams cross the syscall boundary, not two slightly
// different sets of socket options.
//
// Receiver role: bound to cfg.port on INADDR_ANY, optional group join.
// Sender role: connected to cfg.dst_ip:cfg.port, except for multicast and
// multi-peer fan-out, which stay unconnected so replies from every receiver
// get in -- the caller then addresses each datagram with UdpSocket::dst.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "dgram_io/backend.h"
#include "dgram_io/pkt.h"

namespace dgram_io {

struct UdpSocket {
  int fd = -1;
  bool connected = false;
  sockaddr_in dst{};  // sender role: the configured destination
};

// Fills *s and returns true, or returns false with *err set and s->fd closed.
inline bool open_udp_socket(const Config& cfg, UdpSocket* s,
                            std::string* err) {
  auto fail = [&](const std::string& why) {
    *err = why;
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    return false;
  };
  s->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s->fd < 0) return fail(std::string("socket: ") + strerror(errno));

  if (cfg.listener) {
    int rcvbuf = 1 << 24;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    const int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg.port);
    if (bind(s->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
      return fail(std::string("bind: ") + strerror(errno));
    if (!cfg.group.empty()) {  // multicast fan-out: join the group
      ip_mreq mr{};
      if (inet_pton(AF_INET, cfg.group.c_str(), &mr.imr_multiaddr) != 1)
        return fail("bad group " + cfg.group);
      mr.imr_interface.s_addr = htonl(INADDR_ANY);
      if (!cfg.mcast_if.empty() &&
          inet_pton(AF_INET, cfg.mcast_if.c_str(), &mr.imr_interface) != 1)
        return fail("bad mcast_if " + cfg.mcast_if);
      if (setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) !=
          0)
        return fail(std::string("IP_ADD_MEMBERSHIP: ") + strerror(errno));
    }
    return true;
  }

  // Sender role.
  int sndbuf = 1 << 22;
  setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  s->dst.sin_family = AF_INET;
  s->dst.sin_port = htons(cfg.port);
  if (inet_pton(AF_INET, cfg.dst_ip.c_str(), &s->dst.sin_addr) != 1)
    return fail("bad dst_ip " + cfg.dst_ip);
  if (pkt::is_mcast(s->dst.sin_addr.s_addr)) {
    in_addr ifaddr{};
    ifaddr.s_addr = htonl(INADDR_ANY);
    if (!cfg.mcast_if.empty() &&
        inet_pton(AF_INET, cfg.mcast_if.c_str(), &ifaddr) != 1)
      return fail("bad mcast_if " + cfg.mcast_if);
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    const uint8_t ttl = 1, loop = 1;
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    // Unconnected: replies come back from receivers' own unicast addresses.
  } else if (cfg.multi_peer) {
    // Unicast replication fan-out: same reason as multicast above -- a
    // connect()ed socket would make the kernel silently discard replies from
    // every receiver except the primary.
  } else {
    if (connect(s->fd, reinterpret_cast<const sockaddr*>(&s->dst),
                sizeof(s->dst)) != 0)
      return fail(std::string("connect: ") + strerror(errno));
    s->connected = true;
  }
  return true;
}

}  // namespace dgram_io
