// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Round-trip estimation that keeps a rolling MINIMUM of the samples.
//
// Minimum, not mean: a sample taken by a loop that polls sparsely (a control
// socket read every N microseconds, because syscalls cost) carries 0..N of
// polling delay on top of the true path RTT, always in the same direction.
// The floor of the distribution is the path RTT; the mean is not. A rolling
// window rather than an all-time minimum lets the estimate follow a route
// change.
//
// retry_ns() turns it into a retransmit timer for a layer above that repairs
// loss: a fixed retry tuned on a 10 us LAN re-requests packets whose repair
// is still in flight once the path is a 100 us VPC or a millisecond WAN,
// spending the reverse path exactly when it is stressed.
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

  // Retry no earlier than the repair could possibly arrive: one RTT for the
  // request and the retransmit, x1.5 for scheduling/serialization slack, plus
  // a fixed allowance for a sparsely polled peer. Never below the configured
  // base, so on a fast LAN the timer stays what it was configured to be.
  uint64_t retry_ns(uint64_t base_ns) const {
    if (!have()) return base_ns;
    return std::max(base_ns, min_ns() * 3 / 2 + 20'000);
  }

 private:
  uint64_t ring_[kWindow] = {};
  unsigned idx_ = 0;  // wraps harmlessly: kWindow divides 2^32
  int count_ = 0;
};

}  // namespace rtt
}  // namespace dgram_io
