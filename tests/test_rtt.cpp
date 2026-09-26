// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// rtt::Estimator: rolling-min semantics and the retry floor contract.
#include <cstdio>

#include "dgram_io/rtt.h"

using namespace dgram_io;  // NOLINT: test-local convenience

// Not assert(): a -DNDEBUG build would turn every check into nothing and
// still print "all ok".
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      return 1;                                                        \
    }                                                                  \
  } while (0)

int main() {
  rtt::Estimator e;
  const uint64_t base = 100'000;  // 100 us configured floor

  // No samples: base passes through untouched.
  CHECK(!e.have());
  CHECK(e.retry_ns(base) == base);

  // LAN-ish samples with poll jitter: min filters the jitter; the adapted
  // retry stays AT the base (LAN behavior unchanged).
  for (uint64_t s : {15'000ull, 55'000ull, 12'000ull, 40'000ull})
    e.add_sample(s);
  CHECK(e.min_ns() == 12'000);
  CHECK(e.retry_ns(base) == base);  // 12us*1.5+20us = 38us < 100us floor

  // WAN: 20 ms RTT dominates -- retry follows it.
  rtt::Estimator w;
  for (int i = 0; i < 5; ++i) w.add_sample(20'000'000 + i * 500'000);
  CHECK(w.retry_ns(base) == 20'000'000 * 3 / 2 + 20'000);

  // Rolling window: after kWindow fresh (larger) samples, an old outlier
  // min must age out -- the estimate follows route changes.
  rtt::Estimator r;
  r.add_sample(1'000);
  for (int i = 0; i < rtt::Estimator::kWindow; ++i) r.add_sample(5'000'000);
  CHECK(r.min_ns() == 5'000'000);

  printf("test_rtt: all ok\n");
  return 0;
}
