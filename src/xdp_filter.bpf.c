// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// XDP program for the AF_XDP datagram backend (--io xdp):
// steer exactly our UDP flow into the XSK, leave the kernel stack alone for
// everything else. IPv4 + UDP + dst port == port_map[0] -> redirect to the
// socket bound to this RX queue; anything else (ARP, ICMP, ssh, other UDP
// ports) -> XDP_PASS. The redirect's fallback is also XDP_PASS, so a stale
// attached program with no live socket is harmless -- packets just take the
// normal kernel path.
//
// The TCP backend needs a different filter and lives in its own object
// (xdp_tcp_filter.bpf.c): libxdp selects a program by *section* name, and two
// SEC("xdp") programs in one object cannot be told apart.
//
// Built with: clang -O2 -g -target bpf -c xdp_filter.bpf.c
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 64);  // indexed by rx queue
  __type(key, __u32);
  __type(value, __u32);
} xsks_map SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, __u32);  // UDP port, network byte order
} port_map SEC(".maps");

SEC("xdp")
int xdp_udp_filter(struct xdp_md* ctx) {
  void* data = (void*)(long)ctx->data;
  void* end = (void*)(long)ctx->data_end;

  struct ethhdr* eth = data;
  if ((void*)(eth + 1) > end) return XDP_PASS;
  if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

  struct iphdr* ip = (void*)(eth + 1);
  if ((void*)(ip + 1) > end) return XDP_PASS;
  if (ip->ihl != 5) return XDP_PASS;  // options: not our traffic
  if (ip->protocol != IPPROTO_UDP) return XDP_PASS;

  struct udphdr* udp = (void*)(ip + 1);
  if ((void*)(udp + 1) > end) return XDP_PASS;

  __u32 key = 0;
  __u32* port = bpf_map_lookup_elem(&port_map, &key);
  if (!port || udp->dest != (__be16)*port) return XDP_PASS;

  return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
