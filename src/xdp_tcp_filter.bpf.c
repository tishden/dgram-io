// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// XDP program for the TCP-over-AF_XDP backend (--io tcp-xdp): IPv4 + TCP +
// (src or dst port == port_map[0]) into the XSK, **plus ARP**. Everything
// else -> XDP_PASS.
//
// Two differences from the UDP filter, both forced by TCP:
//   * the client's socket has an ephemeral local port, so only one of the two
//     ports is ever ours -- matching on dst alone would drop every packet the
//     server sends back;
//   * the userspace stack does its own ARP, so ARP frames have to come to us
//     rather than to the kernel. That means the kernel netdev on this
//     interface stops seeing ARP while we are attached: fine on the dedicated
//     point-to-point port the bench uses, and a reason not to attach this one
//     to a shared NIC.
//
// Separate object rather than a second program in xdp_filter.bpf.o: libxdp
// picks a program by section name, and both would be SEC("xdp").
//
// Built with: clang -O2 -g -target bpf -c xdp_tcp_filter.bpf.c
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>

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
  __type(value, __u32);  // TCP port, network byte order
} port_map SEC(".maps");

SEC("xdp")
int xdp_tcp_filter(struct xdp_md* ctx) {
  void* data = (void*)(long)ctx->data;
  void* end = (void*)(long)ctx->data_end;

  struct ethhdr* eth = data;
  if ((void*)(eth + 1) > end) return XDP_PASS;

  if (eth->h_proto == bpf_htons(ETH_P_ARP))
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
  if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

  struct iphdr* ip = (void*)(eth + 1);
  if ((void*)(ip + 1) > end) return XDP_PASS;
  if (ip->ihl != 5) return XDP_PASS;  // options: not our traffic
  if (ip->protocol != IPPROTO_TCP) return XDP_PASS;

  struct tcphdr* tcp = (void*)(ip + 1);
  if ((void*)(tcp + 1) > end) return XDP_PASS;

  __u32 key = 0;
  __u32* port = bpf_map_lookup_elem(&port_map, &key);
  if (!port) return XDP_PASS;
  if (tcp->dest != (__be16)*port && tcp->source != (__be16)*port)
    return XDP_PASS;

  return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
