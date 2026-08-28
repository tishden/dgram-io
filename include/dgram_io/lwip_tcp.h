// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// A TCP endpoint built on lwIP, driven from a poll loop, over a frame
// transport supplied by the caller.
//
// Why the frame transport is abstract: the production user is the DPDK
// backend, where a frame is an rte_mbuf on a vfio-bound 82599. But the same
// code has to be testable without a NIC and without root, so the smoke test
// (test/lwip_smoke.cpp) supplies a socketpair instead and runs the identical
// connect/accept/write/read path between two forked processes. Anything that
// only works with a real NIC underneath is not really tested until the wire is
// free.
//
// Threading: none. lwIP is built with NO_SYS=1, so every entry point below --
// and every lwIP callback it triggers -- runs on the calling thread, inside
// the caller's poll loop. There are no locks because there is nothing else
// running.
#pragma once

#ifdef HAVE_LWIP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dgram_io/backend.h"
#include "dgram_io/stream.h"

struct tcp_pcb;
struct netif;
struct pbuf;

namespace dgram_io {

// The frame transport. Implementations must not block.
class LwipFrameIo {
 public:
  virtual ~LwipFrameIo() = default;
  // One complete Ethernet frame (headers included) out. May stage it; kick()
  // is called at the end of every round that produced traffic.
  virtual bool tx_frame(const uint8_t* data, uint32_t len) = 0;
  virtual void tx_kick() {}
  // Up to `max` frames in, each handed to deliver(ctx, data, len). The data
  // must stay valid for the duration of that call only.
  virtual int rx_frames(int max,
                        void (*deliver)(void*, const uint8_t*, uint32_t),
                        void* ctx) = 0;
  virtual const char* io_name() const { return "frames"; }
};

struct LwipParams {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  std::string ip;                  // our IPv4, dotted
  std::string netmask = "255.255.255.0";
  std::string peer_ip;             // client role: where to connect
  uint16_t port = 0;
  bool server = false;             // sender role listens, receiver connects
  int peers = 1;                   // server: connections to wait for
  uint32_t max_record = 1400;      // largest datagram carried, both ways
  unsigned setup_ms = 30000;       // accept/connect deadline
  bool nodelay = true;
  unsigned seed = 0;               // ISN randomness; 0 = derive from clock
};

class LwipTcp {
 public:
  ~LwipTcp();
  // Brings up the stack and the netif; does not wait for the peer.
  bool init(const LwipParams& p, LwipFrameIo* io, std::string* err);
  // Listens/connects and blocks (polling) until the peers are attached.
  bool wait_ready(std::string* err);

  // Appends one length-prefixed record to a peer's send queue. false = the
  // send window/queue is full; the record was not partially written.
  bool send_record(const void* payload, size_t len);
  bool send_record_to(const void* payload, size_t len, const Endpoint& to);
  // Hands everything queued to the stack and the frames to the wire.
  void flush();
  // Feeds arrived frames into the stack and runs its timers. Safe (and
  // cheap) to call as often as the caller likes.
  void poll();
  // Pops reassembled records; views stay valid until the next take().
  int take(RxPacket* out, int max);

  void log_stats(FILE* f, const char* tag) const;
  int live_peers() const;

 private:
  struct Peer {
    tcp_pcb* pcb = nullptr;
    Endpoint ep;
    Deframer rx;
    bool up = false;
    uint64_t rx_records = 0, tx_records = 0, tx_refused = 0;
    uint64_t rx_backpressure = 0;
  };

  // lwIP C callbacks trampoline back into these.
  static signed char on_accept(void* arg, tcp_pcb* newpcb, signed char err);
  static signed char on_connected(void* arg, tcp_pcb* pcb, signed char err);
  static signed char on_recv(void* arg, tcp_pcb* pcb, pbuf* p, signed char err);
  static void on_err(void* arg, signed char err);
  static signed char on_linkoutput(netif* nif, pbuf* p);
  static signed char on_netif_init(netif* nif);
  static void deliver_frame(void* ctx, const uint8_t* data, uint32_t len);

  Peer* peer_for(tcp_pcb* pcb);
  void attach(tcp_pcb* pcb);

  LwipParams params_;
  LwipFrameIo* io_ = nullptr;
  netif* netif_ = nullptr;          // heap-allocated: lwIP keeps the pointer
  tcp_pcb* listen_pcb_ = nullptr;
  std::vector<Peer> peers_;
  std::vector<uint8_t> scratch_;    // record framing + chained-pbuf flatten
  bool connect_done_ = false;
  bool connect_failed_ = false;
  uint64_t frames_tx_ = 0, frames_rx_ = 0, tx_frame_drops_ = 0;
  uint64_t write_mem_ = 0, closed_ = 0, desyncs_ = 0;
};

}  // namespace dgram_io

#endif  // HAVE_LWIP
