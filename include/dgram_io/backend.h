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
//   dpdk - ixgbe/ena PMD behind vfio-pci, the NIC entirely in user space
//   tcp  - kernel TCP sockets, sender = server / receiver = client
//   tcp-dpdk - the same TCP role split, but the stack is lwIP in our process
//          and the NIC is the same vfio-pci PMD
//   tcp-xdp  - same lwIP stack, frames carried by an XSK instead of a PMD
//
// Contract notes:
//  * queue()/queue_to() copy the payload; the caller may reuse its buffer
//    immediately. Datagrams accumulate (up to an internal batch) and leave on
//    flush() or when the batch fills. Deciding *when* to flush -- the
//    "source drained, send now" policy -- stays with the caller, not here:
//    only the caller knows whether more work is coming.
//  * rx() is non-blocking and returns at most `max` datagrams; the returned
//    views (data pointers and endpoints) stay valid until the next rx() call.
//  * Endpoint carries a MAC because L2 backends address whole frames; the UDP
//    backend leaves it zeroed and ignores it on TX.
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
  bool multi_peer = false;   // sender fans out to several unicast peers: stay
                             // unconnected so replies from every peer get in
  uint16_t port = 0;         // UDP port, host order (both src and dst for xdp)
  std::string mcast_if;      // multicast: egress interface / join interface IP
  std::string group;         // listener: multicast group to join
  // xdp + dpdk
  std::string dst_mac;       // aa:bb:cc:dd:ee:ff; empty = derive/ARP (xdp only)
  // xdp only
  std::string ifname;        // NIC to bind the XSK to
  int queue = 0;             // NIC queue index
  bool force_copy = false;   // skip the XDP_ZEROCOPY attempt
  std::string bpf_obj;       // xdp_filter.bpf.o path; empty = next to binary
  // tcp, tcp-dpdk, tcp-xdp: the sender (listener = false) is the TCP
  // server, the receiver the client. A stream has no datagram boundary, so
  // records carry a 2-byte length prefix (dgram_io/stream.h).
  int tcp_peers = 1;             // sender: connections to wait for at startup
  // accept/connect deadline. Generous on purpose: on the PMD/XSK datapaths
  // each end's bind resets the NIC and retrains the DAC, so the pair can
  // spend tens of seconds settling before a SYN can even cross.
  unsigned tcp_setup_ms = 90000;
  unsigned tcp_busy_poll_us = 0; // kernel tcp: SO_BUSY_POLL (0 = off)
  bool tcp_nodelay = true;       // kernel tcp + lwIP: Nagle off by default
  // dpdk only (the port has no kernel netdev: its identity comes from here)
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
  // false = not accepted: larger than max_datagram, or -- on the backends
  // with a bounded TX ring -- the ring is full right now. The latter is
  // back-pressure: flush(), service rx(), and offer the datagram again.
  virtual bool queue(const void* payload, size_t len) = 0;
  virtual bool queue_to(const void* payload, size_t len,
                        const Endpoint& to) = 0;
  // Hand everything queued to the datapath.
  virtual void flush() = 0;
  // Up to `max` received datagrams into `out`, without blocking. Returns the
  // count (0 = nothing yet) or -1 on a failed datapath; the views stay valid
  // until the next rx().
  virtual int rx(RxPacket* out, int max) = 0;
  virtual const char* name() const = 0;
  // One "<name>: k=v ..." line of counters into f.
  virtual void log_stats(FILE* f) const = 0;
};

// Returns nullptr and fills *err on failure (unknown kind, socket/XSK setup).
std::unique_ptr<Backend> make_backend(const Config& cfg, std::string* err);

// True for the reliable-stream backends (tcp, tcp-dpdk, tcp-xdp). A layer
// above that adds its own loss recovery (retransmission, FEC) or injects loss
// for testing should switch that off here: on a stream there is nothing to
// repair, and it would still change the send path.
bool is_stream_backend(const std::string& kind);

}  // namespace dgram_io
