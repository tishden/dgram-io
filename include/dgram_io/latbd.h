// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Per-stage latency breakdown : where do the microseconds
// go between the producer's timestamp and the consumer's ring?
//
//   sender:   ring_wait  = t_deq - send_ts_ns   (sat in the producer ring)
//             build      = t_queued - t_deq     (framing/FEC/queue, pre-syscall)
//   receiver: to_rx      = t_rx - send_ts_ns    (everything up to backend rx;
//                          cross-host validity = clock sync quality, report
//                          it next to the clock-uncertainty column)
//             publish    = t_enq - t_rx         (unpack + shm publish)
//
// The wire+stack segment is the cross-check: to_rx - (ring_wait + build).
//
// Sampling: every K-th unit (--lat-breakdown K, 0 = off). Cost per sampled
// unit is two clock reads; unsampled units pay one predictable branch.
// Reservoir keeps the first kCap samples -- at K=64 that covers 4M messages,
// more than any single case in the matrix.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace dgram_io {
namespace latbd {

class Stage {
 public:
  static constexpr size_t kCap = 65536;

  explicit Stage(const char* name) : name_(name) { v_.reserve(1024); }

  void add(uint64_t ns) {
    if (v_.size() < kCap) v_.push_back(ns);
    ++total_;
  }

  // "latbd stage=<name> n=... p50_us=... p99_us=... p999_us=... max_us=..."
  void report(FILE* f, const char* who) {
    if (v_.empty()) return;
    std::sort(v_.begin(), v_.end());
    auto pct = [&](double p) {
      const size_t i = static_cast<size_t>(p * (v_.size() - 1));
      return v_[i] / 1000.0;
    };
    fprintf(f,
            "%s: latbd stage=%s n=%llu p50_us=%.2f p99_us=%.2f p999_us=%.2f "
            "max_us=%.2f\n",
            who, name_, (unsigned long long)total_, pct(0.50), pct(0.99),
            pct(0.999), v_.back() / 1000.0);
  }

 private:
  const char* name_;
  std::vector<uint64_t> v_;
  uint64_t total_ = 0;
};

}  // namespace latbd
}  // namespace dgram_io
