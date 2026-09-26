// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Packet I/O behind an interface: the layer above speaks datagrams to a peer
// and never learns what carries them.
//
// Backends:
//   udp  - kernel UDP sockets, sendmmsg/recvmmsg (the default)
//   uring - the same kernel UDP socket, driven through io_uring: one submit
//          per flushed batch, and an idle rx() poll costs no syscall at all
//   xdp  - AF_XDP socket on a NIC queue, kernel stack bypassed (needs root)
//   dpdk - a DPDK poll-mode driver behind vfio-pci, the NIC in user space
//   tcp  - kernel TCP sockets, sender = server / receiver = client
//   tcp-dpdk - the same TCP role split, but the stack is lwIP in our process
//          and the NIC is the same vfio-pci PMD
//   tcp-xdp  - same lwIP stack, frames carried by an XSK instead of a PMD
//
// Contract notes:
//  * queue()/queue_to() copy the payload; the caller may reuse its buffer
//    immediately. Datagrams accumulate (up to an internal batch) and leave on
//    flush() or when the batch fills. Deciding *when* to flush -- the
//    "nothing more to send right now" policy -- stays with the caller, not
//    here: only the caller knows whether more work is coming.
//  * rx() is non-blocking and returns at most `max` datagrams; the returned
//    views (data pointers and endpoints) stay valid until the next rx() call.
//  * Endpoint carries a MAC because L2 backends address whole frames; the
//    socket backends leave it zeroed and ignore it on TX.
//  * One DPDK backend (dpdk or tcp-dpdk) and one lwIP backend (tcp-dpdk or
//    tcp-xdp) per process: EAL and lwIP are process-wide singletons.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "dgram_io/limits.h"  // datagram size default/cap: backends size buffers from it

namespace dgram_io {

struct Endpoint {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  uint32_t ip_be = 0;    // network byte order
  uint16_t port_be = 0;  // network byte order
};

struct RxPacket {
  const uint8_t* data = nullptr;  // UDP payload only (L2/L3 stripped)
  uint32_t len = 0;
  Endpoint from;
};

struct Config {
  std::string kind = "udp";  // udp | uring | xdp | dpdk | tcp | tcp-dpdk | tcp-xdp
  // Largest datagram this backend must carry, both ways. Buffers are sized
  // from it rather than from the compile-time cap, so asking for jumbo costs
  // memory only in the runs that asked. A backend that cannot carry the size
  // requested must fail loudly at setup: truncating on receive would look
  // like packet loss, and whatever recovery layer sits above would quietly
  // repair it, hiding the misconfiguration behind a plausible number.
  uint32_t max_datagram = kDefaultDatagram;
  bool listener = false;     // receiver role: bind/join instead of connect
  std::string dst_ip;        // sender role: where data goes (may be multicast)
  uint16_t port = 0;         // UDP/TCP port, host order (src and dst on L2)
  // udp + uring only; the L2 backends reach a multicast dst_ip by its group
  // MAC and have no join, the stream backends have no multicast at all.
  bool multi_peer = false;   // sender fans out to several unicast peers: stay
                             // unconnected so replies from every peer get in
  std::string mcast_if;      // multicast: egress interface / join interface IP
  std::string group;         // listener: multicast group to join
  // xdp + dpdk
  std::string dst_mac;       // aa:bb:cc:dd:ee:ff; empty = ARP (xdp only)
  // xdp + tcp-xdp
  std::string ifname;        // NIC to bind the XSK to
  int xdp_queue = 0;         // NIC queue index
  bool force_copy = false;   // skip the XDP_ZEROCOPY attempt
  // Path of the XDP program object: xdp_filter.bpf.o for xdp,
  // xdp_tcp_filter.bpf.o for tcp-xdp. Empty = that file next to the running
  // binary, else in the directory `make install` put it in.
  std::string bpf_obj;
  // tcp, tcp-dpdk, tcp-xdp: the sender (listener = false) is the TCP
  // server, the receiver the client. A stream has no datagram boundary, so
  // records carry a 2-byte length prefix (dgram_io/stream.h).
  int tcp_peers = 1;             // sender: connections to wait for at startup
  // accept/connect deadline. Generous on purpose: on the PMD/XSK datapaths
  // each end's bind can reset the NIC and retrain the link, so the pair can
  // spend tens of seconds settling before a SYN can even cross.
  unsigned tcp_setup_ms = 90000;
  unsigned tcp_busy_poll_us = 0; // kernel tcp: SO_BUSY_POLL (0 = off)
  bool tcp_nodelay = true;       // kernel tcp + lwIP: Nagle off by default
  // dpdk + tcp-dpdk (the port has no kernel netdev: its identity comes from
  // here)
  std::string dpdk_pci;      // PCI address to take over (-a allow-list)
  std::string dpdk_vdev;     // virtual device, for a test without a NIC
                             // (net_af_packet,iface=..)
  std::string dpdk_ip;       // our IPv4 for the headers we build
  // uring only
  bool uring_sqpoll = false; // a kernel thread polls the submission queue:
                             // flush() stops being a syscall, at a core's cost
  int uring_sqpoll_cpu = -1; // pin that thread (-1 = let the scheduler place it)
};

class Backend {
 public:
  virtual ~Backend() = default;
  // Copy one datagram into the backend's batch, addressed to the configured
  // destination (queue) or to `to` (queue_to; a reply to RxPacket::from).
  // false = not accepted: larger than max_datagram, no destination (a
  // listener that has not been given one), no live peer left on a stream, or
  // the TX side is full right now. The last is back-pressure: flush(),
  // service rx(), and offer the datagram again. A refused datagram was not
  // sent anywhere, so offering it again never duplicates it.
  virtual bool queue(const void* payload, size_t len) = 0;
  virtual bool queue_to(const void* payload, size_t len,
                        const Endpoint& to) = 0;
  // Hand everything queued to the datapath.
  virtual void flush() = 0;
  // Up to `max` received datagrams into `out`, without blocking. Returns the
  // count (0 = nothing yet) or -1 on a failed datapath (a socket error; on a
  // stream, every connection gone); the views stay valid until the next rx().
  virtual int rx(RxPacket* out, int max) = 0;
  virtual const char* name() const = 0;
  // One "<name>: k=v ..." line of counters into f.
  virtual void log_stats(FILE* f) const = 0;
};

// Returns nullptr and fills *err on failure (unknown kind, max_datagram out of
// range, socket/XSK/port setup).
std::unique_ptr<Backend> make_backend(const Config& cfg, std::string* err);

// True for the reliable-stream backends (tcp, tcp-dpdk, tcp-xdp). A layer
// above that adds its own loss recovery (retransmission, FEC) or injects loss
// for testing should switch that off here: on a stream there is nothing to
// repair, and it would still change the send path.
bool is_stream_backend(const std::string& kind);

}  // namespace dgram_io
