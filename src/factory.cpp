// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// The one place that maps a backend name to an implementation.
//
// Every backend is a separate translation unit, and each is compiled in only
// when its dependency is present (liburing, libxdp, libdpdk, lwIP -- see the Makefile).
// A backend that was compiled out still has its make_*() symbol: the stub in
// its own file returns nullptr with an error naming what is missing, so
// `--io dpdk` on a build without DPDK says so instead of failing to link.
#include "dgram_io/backend.h"

#include <string>

namespace dgram_io {

std::unique_ptr<Backend> make_udp_backend(const Config& cfg, std::string* err);
std::unique_ptr<Backend> make_uring_backend(const Config& cfg,
                                            std::string* err);
std::unique_ptr<Backend> make_xdp_backend(const Config& cfg, std::string* err);
std::unique_ptr<Backend> make_dpdk_backend(const Config& cfg, std::string* err);
std::unique_ptr<Backend> make_tcp_backend(const Config& cfg, std::string* err);
std::unique_ptr<Backend> make_tcp_dpdk_backend(const Config& cfg,
                                               std::string* err);
std::unique_ptr<Backend> make_tcp_xdp_backend(const Config& cfg,
                                              std::string* err);

std::unique_ptr<Backend> make_backend(const Config& cfg, std::string* err) {
  if (cfg.kind == "udp") return make_udp_backend(cfg, err);
  if (cfg.kind == "uring") return make_uring_backend(cfg, err);
  if (cfg.kind == "xdp") return make_xdp_backend(cfg, err);
  if (cfg.kind == "dpdk") return make_dpdk_backend(cfg, err);
  if (cfg.kind == "tcp") return make_tcp_backend(cfg, err);
  if (cfg.kind == "tcp-dpdk") return make_tcp_dpdk_backend(cfg, err);
  if (cfg.kind == "tcp-xdp") return make_tcp_xdp_backend(cfg, err);
  *err = "unknown io backend '" + cfg.kind +
         "' (udp|uring|xdp|dpdk|tcp|tcp-dpdk|tcp-xdp)";
  return nullptr;
}

// True for the reliable-stream backends. Callers use it to refuse loss
// recovery (FEC, NACK) and loss injection: those have nothing to repair on a
// stream, but would still change the send path if left enabled.
bool is_stream_backend(const std::string& kind) {
  return kind == "tcp" || kind == "tcp-dpdk" || kind == "tcp-xdp";
}

}  // namespace dgram_io
