# Third-party components

Nothing third-party is vendored in this repository. Everything below is
fetched or linked at build time and stays under its own licence; this file
records what a build actually pulls in.

| Component | Used by | Licence | How it gets here |
|---|---|---|---|
| [lwIP](https://github.com/lwip-tcpip/lwip) 2.2.1 | `tcp-dpdk`, `tcp-xdp` | BSD 3-clause | `scripts/get_lwip.sh` clones the pinned tag into `third_party/lwip` |
| [liburing](https://github.com/axboe/liburing) | `uring` | MIT OR LGPL-2.1 | distro package, linked dynamically |
| [libxdp / libbpf](https://github.com/xdp-project/xdp-tools) | `xdp`, `tcp-xdp` | LGPL-2.1 / BSD-2-Clause (libbpf: LGPL-2.1 OR BSD-2-Clause) | distro package, linked dynamically |
| [DPDK](https://www.dpdk.org/) | `dpdk`, `tcp-dpdk` | BSD 3-clause | distro package, linked dynamically |

Notes on how that interacts with this project's Apache-2.0 licence:

* **lwIP** is BSD-3, which is Apache-2.0 compatible. It is not copied into this
  tree, so nothing here relicenses it. `src/lwip/` holds only the port glue
  (`lwipopts.h`, `arch/cc.h`, `lwip_port.c`) — that glue is original work under
  Apache-2.0, even though it exists solely to configure lwIP.
* **libxdp** is LGPL-2.1 and is linked dynamically. If you statically link it,
  the LGPL's relinking obligations apply to your distribution, not to this
  source tree.
* **DPDK** is BSD-3 and Apache-2.0 compatible.

None of the four is required. `make config` prints what the current machine
has; a build with none of them still gives you the `udp` and `tcp` backends,
which need nothing but the kernel.
