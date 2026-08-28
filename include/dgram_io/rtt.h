// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// RTT estimation for the NACK timers.
//
// Why: the retry cadence and the sender's history depth were tuned on a
// wire with RTT ~10 µs. On a VPC (50-100+ µs) or a real WAN (ms), a fixed
// 100 µs retry re-requests packets whose retransmit is still in flight --
// wasted reverse-path bandwidth and duplicate repairs at exactly the moment
// the channel is stressed.
//
// How: the receiver stamps echo requests, the sender reflects them, and the
// receiver keeps a rolling MINIMUM of the samples. Minimum, not mean: the
// sender only polls its control socket every --nack-poll-us (deliberately
// sparse, syscalls cost p50), so every sample carries 0..poll_us of that
// jitter on top of the true path RTT. The floor of the distribution is the
// path RTT; the mean is not. A rolling window (not an all-time min) lets the
// estimate follow route changes.
//
// The adapted retry interval never goes BELOW the configured base: on a
// fast LAN the estimator converges to the old fixed behavior, so local
// results stay comparable.
#pragma once

#include <algorithm>
#include <cstdint>

namespace dgram_io {
namespace rtt {

class Estimator {
 public:
  static constexpr int kWindow = 32;

  void add_sample(uint64_t sample_ns) {
    ring_[idx_++ % kWindow] = sample_ns;
    if (count_ < kWindow) ++count_;
  }

  bool have() const { return count_ > 0; }

  uint64_t min_ns() const {
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < count_; ++i) m = std::min(m, ring_[i]);
    return m == UINT64_MAX ? 0 : m;
  }

  // Retry no earlier than the repair could possibly arrive: one RTT for
  // NACK+retransmit, x1.5 for scheduling/serialization slack, plus a fixed
  // floor for the sender's sparse control poll. Never below the configured
  // base -- LAN behavior is unchanged.
  uint64_t retry_ns(uint64_t base_ns) const {
    if (!have()) return base_ns;
    return std::max(base_ns, min_ns() * 3 / 2 + 20'000);
  }

 private:
  uint64_t ring_[kWindow] = {};
  int idx_ = 0;
  int count_ = 0;
};

}  // namespace rtt
}  // namespace dgram_io
