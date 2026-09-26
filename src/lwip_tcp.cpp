// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// lwIP endpoint driven from our poll loop. See lwip_tcp.h for the shape.
#include "lwip_tcp.h"

#ifdef HAVE_LWIP

// No <arpa/inet.h> here: lwIP defines its own htons/ntohl macros and the two
// headers fight over them. Everything we need (address parsing/printing, byte
// order) comes from the stack's own API.
#include <time.h>

#include <cstdlib>
#include <cstring>

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

extern "C" void dgram_io_lwip_seed(unsigned seed);

namespace dgram_io {

namespace {

// One RX burst handed to the stack per poll round. Deep enough that a 10G
// line-rate burst between two polls does not sit in the NIC ring, shallow
// enough that we return to the caller (and its own queue) promptly.
constexpr int kRxBurst = 64;

// lwIP is a per-process singleton: its PCB lists, memory pools and timer
// wheel are file-scope state. One endpoint per process is all we need (the
// sender and the receiver are separate processes), and this makes the
// assumption explicit instead of letting a second instance quietly corrupt
// the first one's lists.
LwipTcp* g_only = nullptr;

uint64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

LwipTcp::~LwipTcp() {
  for (Peer& p : peers_)
    if (p.pcb) tcp_abort(p.pcb);
  if (listen_pcb_) tcp_close(listen_pcb_);
  if (netif_) {
    netif_remove(netif_);
    delete netif_;
  }
  if (g_only == this) g_only = nullptr;
}

err_t LwipTcp::on_netif_init(netif* nif) {
  nif->name[0] = 'd';
  nif->name[1] = 'p';
  nif->output = etharp_output;   // IPv4 -> ARP -> linkoutput
  nif->linkoutput = &LwipTcp::on_linkoutput;
  nif->mtu = 1500;
  nif->hwaddr_len = ETH_HWADDR_LEN;
  LwipTcp* self = static_cast<LwipTcp*>(nif->state);
  std::memcpy(nif->hwaddr, self->params_.mac, ETH_HWADDR_LEN);
  nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
  return ERR_OK;
}

err_t LwipTcp::on_linkoutput(netif* nif, pbuf* p) {
  LwipTcp* self = static_cast<LwipTcp*>(nif->state);
  const uint8_t* data;
  if (p->next == nullptr) {  // the common case: LWIP_NETIF_TX_SINGLE_PBUF
    data = static_cast<const uint8_t*>(p->payload);
  } else {  // ARP replies and ICMP can still arrive chained
    if (self->scratch_.size() < p->tot_len) self->scratch_.resize(p->tot_len);
    pbuf_copy_partial(p, self->scratch_.data(), p->tot_len, 0);
    data = self->scratch_.data();
  }
  if (!self->io_->tx_frame(data, p->tot_len)) {
    ++self->tx_frame_drops_;
    return ERR_IF;  // lwIP will retransmit; a dropped frame is not a desync
  }
  ++self->frames_tx_;
  return ERR_OK;
}

void LwipTcp::deliver_frame(void* ctx, const uint8_t* data, uint32_t len) {
  LwipTcp* self = static_cast<LwipTcp*>(ctx);
  pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(len), PBUF_POOL);
  if (!p) {  // pool exhausted: counted by lwIP's own pbuf stats too
    if (self->pool_dry_++ == 0 && self->trace_) self->trace("pool-dry");
    return;
  }
  pbuf_take(p, data, static_cast<u16_t>(len));
  ++self->frames_rx_;
  if (self->netif_->input(p, self->netif_) != ERR_OK) pbuf_free(p);
}

LwipTcp::Peer* LwipTcp::peer_for(tcp_pcb* pcb) {
  for (Peer& p : peers_)
    if (p.pcb == pcb) return &p;
  return nullptr;
}

void LwipTcp::attach(tcp_pcb* pcb) {
  peers_.emplace_back();
  Peer& p = peers_.back();
  p.pcb = pcb;
  p.up = true;
  p.rx.init(params_.max_record, 4u << 20);
  p.ep.ip_be = ip_2_ip4(&pcb->remote_ip)->addr;
  p.ep.port_be = lwip_htons(pcb->remote_port);
  std::memcpy(p.ep.mac, params_.mac, 6);  // L2 is the stack's business now
  tcp_arg(pcb, &p);
  tcp_recv(pcb, &LwipTcp::on_recv);
  tcp_err(pcb, &LwipTcp::on_err);
  if (params_.nodelay) tcp_nagle_disable(pcb);
  // The default poll callback interval is fine; we never rely on it, but
  // lwIP wants tcp_poll set for zero-window probing to be scheduled.
  tcp_poll(pcb, nullptr, 4);
}

err_t LwipTcp::on_accept(void* arg, tcp_pcb* newpcb, err_t err) {
  LwipTcp* self = static_cast<LwipTcp*>(arg);
  if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;
  if (static_cast<int>(self->peers_.size()) >= self->params_.peers) {
    tcp_abort(newpcb);  // the peer set is fixed at startup; no late joiners
    return ERR_ABRT;
  }
  self->attach(newpcb);
  fprintf(stderr, "io_lwip: peer %zu connected from %s:%u\n",
          self->peers_.size(), ip4addr_ntoa(ip_2_ip4(&newpcb->remote_ip)),
          newpcb->remote_port);
  return ERR_OK;
}

err_t LwipTcp::on_connected(void* arg, tcp_pcb* pcb, err_t err) {
  Peer* p = static_cast<Peer*>(arg);
  (void)pcb;
  if (err != ERR_OK) return err;
  p->up = true;
  return ERR_OK;
}

err_t LwipTcp::on_recv(void* arg, tcp_pcb* pcb, pbuf* p, err_t err) {
  Peer* peer = static_cast<Peer*>(arg);
  LwipTcp* self = g_only;
  if (p == nullptr) {  // peer closed its half
    peer->up = false;
    if (self) ++self->closed_;
    return ERR_OK;
  }
  if (err != ERR_OK) {
    pbuf_free(p);
    return err;
  }
  // Back-pressure, done properly: refusing the pbuf (without freeing it)
  // leaves the data in lwIP's refused_data and keeps the advertised window
  // closed until we drain. Copying into a buffer we cannot deliver from
  // would silently turn a slow consumer into unbounded memory growth.
  if (peer->rx.write_space() < p->tot_len) {
    ++peer->rx_backpressure;
    return ERR_MEM;
  }
  pbuf_copy_partial(p, peer->rx.write_ptr(), p->tot_len, 0);
  peer->rx.committed(p->tot_len);
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

void LwipTcp::on_err(void* arg, err_t err) {
  Peer* peer = static_cast<Peer*>(arg);
  if (g_only && g_only->trace_)
    fprintf(stderr, "io_lwip_trace: on_err err=%d (pcb already freed)\n",
            static_cast<int>(err));
  const bool was_up = peer->up;
  peer->up = false;
  peer->pcb = nullptr;  // lwIP already freed it
  if (g_only) ++g_only->closed_;
  // A connect retry against a server that is not listening yet earns a RST
  // every 250 ms, which is normal and would otherwise bury the real
  // diagnostics under hundreds of identical lines. Only a connection that was
  // actually established is worth a word.
  if (was_up)
    fprintf(stderr, "io_lwip: connection error %d\n", static_cast<int>(err));
}

bool LwipTcp::init(const LwipParams& p, LwipFrameIo* io, std::string* err) {
  if (g_only) {
    *err = "lwIP is a per-process singleton; one endpoint per process";
    return false;
  }
  params_ = p;
  io_ = io;
  const char* tr = getenv("DGRAM_IO_LWIP_TRACE");
  trace_ = tr && *tr && *tr != '0';
  scratch_.resize(2048);
  peers_.reserve(p.server ? (p.peers < 1 ? 1 : p.peers) : 1);

  dgram_io_lwip_seed(p.seed ? p.seed : static_cast<unsigned>(now_ms()));
  lwip_init();
  g_only = this;

  ip4_addr_t ip{}, mask{}, gw{};
  if (!ip4addr_aton(params_.ip.c_str(), &ip)) {
    *err = "bad local IPv4 address '" + params_.ip + "'";
    return false;
  }
  if (!ip4addr_aton(params_.netmask.c_str(), &mask)) {
    *err = "bad netmask " + params_.netmask;
    return false;
  }
  netif_ = new netif();
  if (!netif_add(netif_, &ip, &mask, &gw, this, &LwipTcp::on_netif_init,
                 ethernet_input)) {
    *err = "netif_add failed";
    return false;
  }
  netif_set_default(netif_);
  netif_set_up(netif_);
  netif_set_link_up(netif_);
  return true;
}

bool LwipTcp::wait_ready(std::string* err) {
  const uint64_t deadline = now_ms() + params_.setup_ms;
  if (params_.server) {
    tcp_pcb* pcb = tcp_new();
    if (!pcb) {
      *err = "tcp_new failed";
      return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, params_.port) != ERR_OK) {
      *err = "tcp_bind :" + std::to_string(params_.port) + " failed";
      return false;
    }
    listen_pcb_ = tcp_listen_with_backlog(pcb, params_.peers + 2);
    if (!listen_pcb_) {
      *err = "tcp_listen failed";
      return false;
    }
    tcp_arg(listen_pcb_, this);
    tcp_accept(listen_pcb_, &LwipTcp::on_accept);
    fprintf(stderr, "io_lwip: listening on :%u for %d peer(s)\n", params_.port,
            params_.peers);
    while (static_cast<int>(peers_.size()) < params_.peers) {
      poll();
      if (now_ms() > deadline) {
        *err = "timed out waiting for " + std::to_string(params_.peers) +
               " peer(s) on port " + std::to_string(params_.port);
        return false;
      }
    }
    return true;
  }

  ip4_addr_t dst{};
  if (!ip4addr_aton(params_.peer_ip.c_str(), &dst)) {
    *err = "bad server address " + params_.peer_ip;
    return false;
  }
  // One connect attempt, then keep polling: unlike a kernel socket there is
  // nothing to retry against -- if the server is not listening yet its stack
  // answers the SYN with a RST, lwIP reports it through on_err, and we open a
  // fresh pcb. When the receiver is started first, this loop runs for as
  // long as it takes the sender to start listening.
  for (;;) {
    peers_.clear();
    peers_.emplace_back();
    Peer& p = peers_.back();
    p.rx.init(params_.max_record, 4u << 20);
    p.pcb = tcp_new();
    if (!p.pcb) {
      *err = "tcp_new failed";
      return false;
    }
    tcp_arg(p.pcb, &p);
    tcp_err(p.pcb, &LwipTcp::on_err);
    tcp_recv(p.pcb, &LwipTcp::on_recv);
    if (params_.nodelay) tcp_nagle_disable(p.pcb);
    p.ep.ip_be = dst.addr;
    p.ep.port_be = lwip_htons(params_.port);
    connect_done_ = false;
    if (tcp_connect(p.pcb, &dst, params_.port, &LwipTcp::on_connected) !=
        ERR_OK) {
      *err = "tcp_connect failed";
      return false;
    }
    const uint64_t attempt_until = now_ms() + 250;
    while (!peers_[0].up && peers_[0].pcb != nullptr &&
           now_ms() < attempt_until)
      poll();
    if (peers_[0].up) {
      fprintf(stderr, "io_lwip: connected to %s:%u\n", params_.peer_ip.c_str(),
              params_.port);
      return true;
    }
    if (peers_[0].pcb) tcp_abort(peers_[0].pcb);
    if (now_ms() > deadline) {
      *err = "connect " + params_.peer_ip + ":" + std::to_string(params_.port) +
             " timed out";
      return false;
    }
  }
}

bool LwipTcp::write_record(Peer& p, const void* payload, size_t len) {
  const size_t total = kRecPrefix + len;
  if (scratch_.size() < total) scratch_.resize(total);
  scratch_[0] = static_cast<uint8_t>(len & 0xff);
  scratch_[1] = static_cast<uint8_t>(len >> 8);
  if (len) std::memcpy(scratch_.data() + kRecPrefix, payload, len);
  // tcp_write either queues all of it or none of it, so a failure here never
  // leaves half a record in the stream.
  if (tcp_write(p.pcb, scratch_.data(), static_cast<u16_t>(total),
                TCP_WRITE_FLAG_COPY) != ERR_OK) {
    ++p.tx_refused;
    ++write_mem_;
    return false;
  }
  ++p.tx_records;
  return true;
}

bool LwipTcp::send_record(const void* payload, size_t len) {
  // Fan-out is all-or-nothing, like the kernel tcp backend: false means the
  // caller offers the same record again, so no peer may have taken it. Room
  // is checked on every peer before anything is written.
  const size_t total = kRecPrefix + len;
  int live = 0;
  for (Peer& p : peers_) {
    if (!p.up) continue;
    ++live;
    if (tcp_sndbuf(p.pcb) < total) {
      ++p.tx_refused;
      return false;
    }
  }
  if (live == 0) return false;
  bool written = false;
  for (Peer& p : peers_) {
    if (!p.up) continue;
    if (write_record(p, payload, len)) {
      written = true;
      continue;
    }
    // Refused despite the room check (the segment pool ran dry). Before any
    // peer took the record, refusing it is still clean. After, this peer's
    // stream would silently miss one record, and a reliable stream that loses
    // data is worse than a closed one: drop the peer.
    if (!written) return false;
    fprintf(stderr, "io_lwip: peer %s:%u dropped: out of stack memory mid fan-out\n",
            ip4addr_ntoa(ip_2_ip4(&p.pcb->remote_ip)), p.pcb->remote_port);
    drop_peer(p);
  }
  return true;
}

bool LwipTcp::send_record_to(const void* payload, size_t len,
                             const Endpoint& to) {
  for (Peer& p : peers_) {
    if (!p.up) continue;
    if (peers_.size() > 1 &&
        (p.ep.ip_be != to.ip_be || p.ep.port_be != to.port_be))
      continue;
    if (tcp_sndbuf(p.pcb) < kRecPrefix + len) {
      ++p.tx_refused;
      return false;
    }
    return write_record(p, payload, len);
  }
  return false;
}

void LwipTcp::drop_peer(Peer& p) {
  p.up = false;  // before the abort, so on_err does not report it again
  if (p.pcb) {
    tcp_abort(p.pcb);  // runs on_err, which clears p.pcb
    p.pcb = nullptr;
  }
}

void LwipTcp::flush() {
  for (Peer& p : peers_)
    if (p.up && p.pcb) tcp_output(p.pcb);
  io_->tx_kick();
}

void LwipTcp::poll() {
  io_->rx_frames(kRxBurst, &LwipTcp::deliver_frame, this);
  sys_check_timeouts();
  if (trace_ && now_ms() >= trace_next_ms_) {
    trace_next_ms_ = now_ms() + 1000;
    trace("tick");
  }
  // Input generates output: ACKs, window updates, ARP replies and any
  // retransmit the timers decided on are all sitting in the frame transport's
  // staging area now. The receiver never calls flush() -- it has nothing to
  // send -- so if this kick were left to the caller its ACKs would leave one
  // poll round late, which the sender would see as a stalled window.
  io_->tx_kick();
}

int LwipTcp::take(RxPacket* out, int max) {
  for (Peer& p : peers_) p.rx.compact();  // previous batch's views die here
  poll();
  int emitted = 0;
  for (Peer& p : peers_) {
    bool bad = false;
    while (emitted < max) {
      const uint8_t* d;
      uint32_t len;
      if (!p.rx.next(&d, &len, &bad)) break;
      out[emitted].data = d;
      out[emitted].len = len;
      out[emitted].from = p.ep;
      ++emitted;
      ++p.rx_records;
    }
    if (bad) {
      ++desyncs_;
      fprintf(stderr, "io_lwip: framing error from a peer, connection dropped\n");
      drop_peer(p);
    }
  }
  // No live peer and nothing left to hand out: the datapath is dead, which
  // the caller must be able to tell from "nothing yet".
  if (emitted == 0 && live_peers() == 0) return -1;
  return emitted;
}

namespace {
int seg_count(const tcp_seg* s, int* pbufs) {
  int n = 0;
  for (; s; s = s->next) {
    ++n;
    if (pbufs) *pbufs += pbuf_clen(s->p);
  }
  return n;
}
}  // namespace

void LwipTcp::trace(const char* why) {
  const stats_mem* pool = lwip_stats.memp[MEMP_PBUF_POOL];
  const stats_mem* seg = lwip_stats.memp[MEMP_TCP_SEG];
  fprintf(stderr,
          "io_lwip_trace: %s t=%llu %s pool=%u/%u max=%u err=%u seg=%u max=%u "
          "err=%u heap=%u err=%u frames_rx=%llu frames_tx=%llu pool_dry=%llu\n",
          why, (unsigned long long)now_ms(), params_.server ? "server" : "client",
          (unsigned)pool->used, (unsigned)PBUF_POOL_SIZE, (unsigned)pool->max,
          (unsigned)pool->err, (unsigned)seg->used, (unsigned)seg->max,
          (unsigned)seg->err, (unsigned)lwip_stats.mem.used,
          (unsigned)lwip_stats.mem.err, (unsigned long long)frames_rx_,
          (unsigned long long)frames_tx_, (unsigned long long)pool_dry_);
  for (const Peer& p : peers_) {
    const tcp_pcb* c = p.pcb;
    if (!c) {
      fprintf(stderr, "io_lwip_trace:   peer up=%d pcb=gone\n", p.up ? 1 : 0);
      continue;
    }
    int ooseq_pbufs = 0;
#if TCP_QUEUE_OOSEQ
    const int ooseq = seg_count(c->ooseq, &ooseq_pbufs);
#else
    const int ooseq = 0;
#endif
    fprintf(stderr,
            "io_lwip_trace:   peer up=%d state=%d snd_queuelen=%u unsent=%d "
            "unacked=%d ooseq=%d/%dpbufs refused=%d nrtx=%u rto=%d rcv_wnd=%u "
            "snd_wnd=%u snd_buf=%u rx_records=%llu tx_records=%llu\n",
            p.up ? 1 : 0, static_cast<int>(c->state),
            (unsigned)c->snd_queuelen, seg_count(c->unsent, nullptr),
            seg_count(c->unacked, nullptr), ooseq, ooseq_pbufs,
            c->refused_data ? 1 : 0, (unsigned)c->nrtx, (int)c->rto,
            (unsigned)c->rcv_wnd, (unsigned)c->snd_wnd, (unsigned)c->snd_buf,
            (unsigned long long)p.rx_records, (unsigned long long)p.tx_records);
  }
}

int LwipTcp::live_peers() const {
  int n = 0;
  for (const Peer& p : peers_) n += p.up ? 1 : 0;
  return n;
}

void LwipTcp::log_stats(FILE* f, const char* tag) const {
  uint64_t txr = 0, rxr = 0, refused = 0, bp = 0;
  for (const Peer& p : peers_) {
    txr += p.tx_records;
    rxr += p.rx_records;
    refused += p.tx_refused;
    bp += p.rx_backpressure;
  }
  fprintf(f,
          "%s: role=%s peers=%zu live=%d tx_records=%llu rx_records=%llu "
          "tx_refused=%llu rx_backpressure=%llu frames_tx=%llu frames_rx=%llu "
          "tx_frame_drops=%llu write_mem=%llu closed=%llu desyncs=%llu\n",
          tag, params_.server ? "server" : "client", peers_.size(),
          live_peers(), (unsigned long long)txr, (unsigned long long)rxr,
          (unsigned long long)refused, (unsigned long long)bp,
          (unsigned long long)frames_tx_, (unsigned long long)frames_rx_,
          (unsigned long long)tx_frame_drops_, (unsigned long long)write_mem_,
          (unsigned long long)closed_, (unsigned long long)desyncs_);
#if TCP_STATS
  // The stack's own view. lwIP keeps no retransmission counter, so the way to
  // tell "TCP was slow" from "our datapath was slow" on a clean link is
  // tcp_xmit against the NIC's opackets (equal = nothing was sent twice) plus
  // the error columns below being zero. mem_err / pbuf_err are the ones that
  // silently cost latency: they mean the stack refused work for lack of a
  // buffer and fell back on a timer.
  fprintf(f,
          "%s_stack: tcp_xmit=%u tcp_recv=%u tcp_drop=%u tcp_chkerr=%u "
          "tcp_lenerr=%u tcp_memerr=%u tcp_proterr=%u tcp_rterr=%u "
          "ip_drop=%u ip_chkerr=%u link_drop=%u pbuf_err=%u mem_err=%u "
          "mem_max=%u\n",
          tag, (unsigned)lwip_stats.tcp.xmit, (unsigned)lwip_stats.tcp.recv,
          (unsigned)lwip_stats.tcp.drop, (unsigned)lwip_stats.tcp.chkerr,
          (unsigned)lwip_stats.tcp.lenerr, (unsigned)lwip_stats.tcp.memerr,
          (unsigned)lwip_stats.tcp.proterr, (unsigned)lwip_stats.tcp.rterr,
          (unsigned)lwip_stats.ip.drop, (unsigned)lwip_stats.ip.chkerr,
          (unsigned)lwip_stats.link.drop,
          (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err,
          (unsigned)lwip_stats.mem.err, (unsigned)lwip_stats.mem.max);
#endif
}

}  // namespace dgram_io

#endif  // HAVE_LWIP
