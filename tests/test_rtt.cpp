// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// rtt::Estimator: rolling-min semantics and the retry floor contract.
#include <cassert>
#include <cstdio>

#include "dgram_io/rtt.h"

using namespace dgram_io;  // NOLINT: test-local convenience

int main() {
  rtt::Estimator e;
  const uint64_t base = 100'000;  // 100 us configured floor

  // No samples: base passes through untouched.
  assert(!e.have());
  assert(e.retry_ns(base) == base);

  // LAN-ish samples with poll jitter: min filters the jitter; the adapted
  // retry stays AT the base (LAN behavior unchanged).
  for (uint64_t s : {15'000ull, 55'000ull, 12'000ull, 40'000ull})
    e.add_sample(s);
  assert(e.min_ns() == 12'000);
  assert(e.retry_ns(base) == base);  // 12us*1.5+20us = 38us < 100us floor

  // WAN: 20 ms RTT dominates -- retry follows it.
  rtt::Estimator w;
  for (int i = 0; i < 5; ++i) w.add_sample(20'000'000 + i * 500'000);
  assert(w.retry_ns(base) == 20'000'000 * 3 / 2 + 20'000);

  // Rolling window: after kWindow fresh (larger) samples, an old outlier
  // min must age out -- the estimate follows route changes.
  rtt::Estimator r;
  r.add_sample(1'000);
  for (int i = 0; i < rtt::Estimator::kWindow; ++i) r.add_sample(5'000'000);
  assert(r.min_ns() == 5'000'000);

  printf("test_rtt: all ok\n");
  return 0;
}
