// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// The one function lwIP needs from the port with NO_SYS=1 and timers enabled:
// a millisecond clock. Everything else it asks for on a hosted platform comes
// from libc via arch/cc.h.
#include <stdlib.h>
#include <time.h>

#include "lwip/sys.h"

u32_t sys_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// Seeds LWIP_RAND (initial sequence numbers). Called from the C++ side before
// lwip_init(); a fixed seed would make two runs on the same host pick the same
// ISN, which TIME_WAIT on the peer then rejects.
void transport_lwip_seed(unsigned seed) { srandom(seed); }
