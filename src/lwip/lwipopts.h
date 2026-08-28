// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// lwIP configuration for the --io tcp-dpdk backend.
//
// Everything here is chosen for one workload: a single long-lived TCP
// connection carrying a continuous one-way stream of small records over a
// point-to-point 10G link with no loss and ~10 us of round trip, polled from a
// single thread that owns the NIC. That is the opposite of lwIP's usual home
// (a microcontroller with 32 KB of RAM and an interrupt-driven driver), so
// almost every size below is raised and almost every OS integration is
// switched off.
//
// The four decisions that matter:
//  1. NO_SYS=1 -- no threads, no mailboxes, no locking. The stack runs
//     entirely inside our poll loop: we feed it frames, it calls our
//     callbacks, we call sys_check_timeouts(). This is what keeps a TCP stack
//     compatible with a busy-poll datapath; the sequential/socket APIs would
//     put a queue and a context switch in the middle.
//  2. Window scaling on, windows in megabytes. The default 64 KB window caps
//     a 10 us-RTT link far below line rate, and it is the first thing to hit
//     when a "TCP is slow" measurement is really "the window was 64 KB".
//  3. TX single pbuf. Each segment leaves as one contiguous buffer, which is
//     exactly what an mbuf wants -- no chain walking in linkoutput.
//  4. Statistics on. They cost a few increments per packet and are the only
//     honest way to say what the stack did (retransmits, out-of-order,
//     memory-pool failures) instead of guessing from latency curves.
#pragma once

// ---- OS integration ------------------------------------------------------
#define NO_SYS                     1
#define SYS_LIGHTWEIGHT_PROT       0
#define LWIP_NETCONN               0
#define LWIP_SOCKET                0
#define LWIP_TIMERS                1   // we drive them from sys_check_timeouts

// ---- protocol surface ----------------------------------------------------
// Only what the point-to-point wire needs. ICMP stays on: being able to ping
// the userspace stack from the peer is worth 2 KB when something is wrong.
#define LWIP_IPV4                  1
#define LWIP_IPV6                  0
#define LWIP_TCP                   1
#define LWIP_UDP                   0
#define LWIP_RAW                   0
#define LWIP_ICMP                  1
#define LWIP_ARP                   1
#define LWIP_ETHERNET              1
#define LWIP_DHCP                  0
#define LWIP_DNS                   0
#define LWIP_AUTOIP                0
#define LWIP_IGMP                  0
#define LWIP_NETIF_HOSTNAME        0
#define LWIP_NETIF_API             0
#define LWIP_NETIF_LOOPBACK        0
#define LWIP_SINGLE_NETIF          1   // one port per process
#define ARP_TABLE_SIZE             4
#define ARP_QUEUEING               1   // hold the SYN while the peer is ARPed

// ---- memory --------------------------------------------------------------
// The heap backs PBUF_RAM, which is where tcp_write(..., COPY) puts payload.
// It has to cover everything unacknowledged plus what we hand it in one
// batch; 16 MB is ~100x the bandwidth-delay product of this link, so the
// first-fit allocator never has to work hard.
#define MEM_LIBC_MALLOC            0
#define MEM_ALIGNMENT              8
#define MEM_SIZE                   (16 * 1024 * 1024)
#define MEMP_MEM_MALLOC            0
#define MEMP_NUM_TCP_PCB           8
#define MEMP_NUM_TCP_PCB_LISTEN    2
#define MEMP_NUM_TCP_SEG           8192
#define MEMP_NUM_ARP_QUEUE         16
#define MEMP_NUM_SYS_TIMEOUT       8
// RX path: one pooled pbuf per received frame, copied out and freed within
// the same poll round, so the pool only has to cover a single RX burst.
#define PBUF_POOL_SIZE             2048
#define PBUF_POOL_BUFSIZE          1600
#define PBUF_LINK_HLEN             14

// ---- TCP -----------------------------------------------------------------
#define TCP_MSS                    1460
#define LWIP_WND_SCALE             1
#define TCP_RCV_SCALE              5           // 65535 << 5 = 2 MB ceiling
// 1 MB each way. The bandwidth-delay product of this link is ~25 KB (10 Gb/s
// x 20 us), so this is 40x the window the transfer can actually fill -- the
// point is only that the window never becomes the limit while we measure the
// stack. lwIP caps the receive window at 0xFFFF << TCP_RCV_SCALE.
#define TCP_WND                    (1024 * 1024)
#define TCP_SND_BUF                (1024 * 1024)
#define TCP_SND_QUEUELEN           (4 * TCP_SND_BUF / TCP_MSS)
// Both low-water marks stay u16_t inside the stack, so they cannot simply
// scale with the (much larger) buffers above; lwIP's sanity check enforces
// 4*MSS of headroom below the overflow. Nothing on our path waits on them --
// we never block on writability -- so the exact values are arbitrary.
#define TCP_SNDLOWAT               32768
#define TCP_SNDQUEUELOWAT          (TCP_SND_QUEUELEN / 4)
#define TCP_LISTEN_BACKLOG         1
#define TCP_DEFAULT_LISTEN_BACKLOG 4
#define TCP_QUEUE_OOSEQ            1
#define LWIP_TCP_SACK_OUT          1
#define TCP_OVERSIZE               TCP_MSS
#define LWIP_TCP_KEEPALIVE         0
#define TCP_TMR_INTERVAL           25          // ms; default 250
// Nagle is disabled per-pcb at connect/accept time (tcp_nagle_disable), not
// here -- lwIP has no global switch, and doing it per pcb keeps the choice
// visible next to the socket options the kernel backend sets.

// ---- checksums -----------------------------------------------------------
// Software checksums on both generate and check. The ixgbe can do IPv4/TCP
// offload, and turning these off is measured separately in the report -- but
// the default has to be the one that is correct on any PMD.
#define CHECKSUM_GEN_IP            1
#define CHECKSUM_GEN_TCP           1
#define CHECKSUM_GEN_ICMP          1
#define CHECKSUM_CHECK_IP          1
#define CHECKSUM_CHECK_TCP         1
#define CHECKSUM_CHECK_ICMP        1
#define LWIP_CHKSUM_ALGORITHM      3   // 32-bit unrolled, the fastest generic

// ---- pbuf/netif behaviour -----------------------------------------------
#define LWIP_NETIF_TX_SINGLE_PBUF  1
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0

// ---- diagnostics ---------------------------------------------------------
#define LWIP_STATS                 1
#define LWIP_STATS_DISPLAY         1
// 32-bit counters. The default STAT_COUNTER is u16_t, which wraps every 65536
// packets -- at 200k msg/s that is every third of a second, and the first
// wire run duly reported a sender that had "transmitted" 1822 segments while
// its NIC counted 198433 frames. Diagnostics that wrap are worse than no
// diagnostics: they look like a finding.
#define LWIP_STATS_LARGE           1
#define TCP_STATS                  1
#define MEM_STATS                  1
#define MEMP_STATS                 1
#define LINK_STATS                 1
#define IP_STATS                   1
#define SYS_STATS                  0
#define LWIP_DEBUG                 0
