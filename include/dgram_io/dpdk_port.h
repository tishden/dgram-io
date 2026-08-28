// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Shared DPDK port bring-up for the two PMD backends: the raw-datagram one
// (dpdk_backend.cpp) and the TCP one (tcp_dpdk_backend.cpp). Both want exactly
// the same thing -- EAL on the core we are already pinned to, one port, one
// RX and one TX queue, an mbuf pool sized to the frames we intend to carry,
// and the link actually up before we declare ourselves ready. Getting any of
// that subtly different between the two would turn a stack comparison into a
// port-configuration comparison.
//
// What stays out: everything above the frame. Header templates, ARP, mbuf
// lifetime policy and the lwIP netif all belong to the backend.
#pragma once

#ifdef HAVE_DPDK

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dgram_io/backend.h"

namespace dgram_io {
namespace dpdkport {

inline constexpr uint16_t kRxDesc = 1024;
inline constexpr uint16_t kTxDesc = 1024;
inline constexpr unsigned kPoolSize = 8191;  // mempool likes 2^n - 1

struct Options {
  uint32_t mtu = 1500;         // L3 MTU to configure on the port
  uint32_t max_frame = 1514;   // largest L2 frame an mbuf must hold
  const char* pool_name = "mb";
  const char* tag = "io_dpdk";  // log prefix, so the two backends stay apart
  bool wait_link = true;
};

struct Port {
  uint16_t id = 0;
  bool eal_up = false;
  bool started = false;
  rte_mempool* pool = nullptr;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  int core = 0;

  void teardown() {
    if (started) {
      rte_eth_dev_stop(id);
      rte_eth_dev_close(id);
      started = false;
    }
    if (eal_up) {
      rte_eal_cleanup();
      eal_up = false;
    }
  }
};

// EAL + port + queues + link. Returns false with *err set; the caller still
// has to call teardown() on whatever came up (Port tracks it).
inline bool setup(const Config& cfg, const Options& opt, Port* p,
                  std::string* err) {
  if (cfg.dpdk_pci.empty() && cfg.dpdk_vdev.empty()) {
    *err = "--io " + cfg.kind + " requires --dpdk-pci ADDR or --dpdk-vdev SPEC";
    return false;
  }

  // EAL on the core we are already pinned to; the caller's taskset is the
  // single source of core placement, EAL just inherits it.
  p->core = sched_getcpu();
  std::vector<std::string> args = {"transport", "--no-telemetry", "-l",
                                   std::to_string(p->core < 0 ? 0 : p->core)};
  if (!cfg.dpdk_pci.empty()) {
    args.push_back("--in-memory");  // no runtime-dir collisions between procs
    args.push_back("-a");
    args.push_back(cfg.dpdk_pci);
  } else {
    // Smoke path: vdev without hugepages. DPDK 23.11 (Ubuntu 24.04) rejects
    // --no-huge combined with --in-memory, so isolate the per-process
    // runtime dir with a unique file-prefix instead -- same effect (two
    // EALs on one host must not share lock files), accepted by 23.11+.
    args.push_back("--no-huge");
    args.push_back("--file-prefix=transport" + std::to_string(getpid()));
    args.push_back("--vdev=" + cfg.dpdk_vdev);
    args.push_back("--no-pci");
  }
  args.push_back("--log-level=lib.eal:warning");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  if (rte_eal_init(static_cast<int>(argv.size()), argv.data()) < 0) {
    *err = std::string("rte_eal_init: ") + rte_strerror(rte_errno);
    return false;
  }
  p->eal_up = true;
  if (rte_eth_dev_count_avail() == 0) {
    *err = "no DPDK port (is " +
           (cfg.dpdk_pci.empty() ? cfg.dpdk_vdev : cfg.dpdk_pci) +
           " bound to vfio-pci? bench/setup_dpdk.sh bind)";
    return false;
  }
  p->id = 0;

  // One mbuf must hold headers + payload: neither backend chains segments on
  // TX, so the buffer grows with the frame rather than the frame being split.
  const uint16_t buf_size = static_cast<uint16_t>(std::max<uint32_t>(
      RTE_MBUF_DEFAULT_BUF_SIZE, RTE_PKTMBUF_HEADROOM + opt.max_frame));
  p->pool = rte_pktmbuf_pool_create(opt.pool_name, kPoolSize, 256, 0, buf_size,
                                    static_cast<int>(rte_socket_id()));
  if (!p->pool) {
    *err = std::string("mbuf pool: ") + rte_strerror(rte_errno);
    return false;
  }

  rte_eth_conf conf{};
  // The port's MTU has to admit the frame as well; leaving it at the default
  // makes the NIC drop what the pool is perfectly able to hold.
  conf.rxmode.mtu = opt.mtu;
  int ret = rte_eth_dev_configure(p->id, 1, 1, &conf);
  // Some PMDs take the MTU from the configure call and reject a separate set;
  // others need the explicit one. Try it and keep going either way -- the
  // size is already carried in the configuration above, and turning a
  // redundant call's refusal into a dead datapath would be worse.
  if (ret == 0 && conf.rxmode.mtu > RTE_ETHER_MTU) {
    const int m =
        rte_eth_dev_set_mtu(p->id, static_cast<uint16_t>(conf.rxmode.mtu));
    if (m != 0)
      fprintf(stderr,
              "%s: set_mtu(%u) refused (%s); relying on the configured value\n",
              opt.tag, conf.rxmode.mtu, rte_strerror(-m));
  }
  if (ret == 0)
    ret = rte_eth_rx_queue_setup(p->id, 0, kRxDesc,
                                 rte_eth_dev_socket_id(p->id), nullptr,
                                 p->pool);
  if (ret == 0)
    ret = rte_eth_tx_queue_setup(p->id, 0, kTxDesc,
                                 rte_eth_dev_socket_id(p->id), nullptr);
  if (ret == 0) ret = rte_eth_dev_start(p->id);
  if (ret != 0) {
    *err = std::string("port setup: ") + rte_strerror(-ret);
    return false;
  }
  p->started = true;

  rte_ether_addr mac;
  rte_eth_macaddr_get(p->id, &mac);
  std::memcpy(p->mac, mac.addr_bytes, 6);

  // Physical ports: wait for the link. Each dev_start resets the 82599, and on
  // this stand the DAC retrain is a roulette -- a good spin links in a few
  // seconds, a wedged one never converges (30 s did not help). So fail fast at
  // 20 s and let the bench driver respin with a fresh process (dev_start);
  // waiting longer on a wedged spin is wasted. Traffic sent into a down link
  // is lost, which the driver detects as hw_ipackets=0 and retries.
  if (opt.wait_link && !cfg.dpdk_pci.empty()) {
    rte_eth_link link{};
    for (int i = 0; i < 200; ++i) {  // <= 20 s
      const int lret = rte_eth_link_get_nowait(p->id, &link);
      if (lret != 0) break;  // PMD without link tracking (vdev): do not spin
      if (link.link_status == RTE_ETH_LINK_UP) break;
      usleep(100 * 1000);
    }
    if (link.link_status != RTE_ETH_LINK_UP)
      fprintf(stderr, "%s: warning: link still down after 20 s\n", opt.tag);
    else
      fprintf(stderr, "%s: link up, %u Mbps %s\n", opt.tag, link.link_speed,
              link.link_duplex ? "full-duplex" : "half-duplex");
  }

  fprintf(stderr, "%s: port=%u dev=%s core=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          opt.tag, p->id,
          cfg.dpdk_pci.empty() ? cfg.dpdk_vdev.c_str() : cfg.dpdk_pci.c_str(),
          p->core, p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4],
          p->mac[5]);
  return true;
}

}  // namespace dpdkport
}  // namespace dgram_io

#endif  // HAVE_DPDK
