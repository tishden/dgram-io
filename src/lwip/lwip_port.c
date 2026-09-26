// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// What lwIP needs from the port with NO_SYS=1 and timers enabled: a
// millisecond clock, plus the random numbers and diagnostics arch/cc.h points
// here. Everything else it asks for on a hosted platform comes from libc.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "lwip/sys.h"

u32_t sys_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// LWIP_RAND (initial sequence numbers, timer jitter). A private xorshift
// rather than random(): srandom() would reseed the application's own
// generator behind its back. Seeded from the C++ side before lwip_init(); a
// fixed seed would make two runs on the same host pick the same ISN, which
// TIME_WAIT on the peer then rejects.
static uint32_t rand_state = 0x9e3779b9u;

void dgram_io_lwip_seed(unsigned seed) { rand_state = seed ? seed : 0x9e3779b9u; }

uint32_t dgram_io_lwip_rand(void) {
  rand_state ^= rand_state << 13;
  rand_state ^= rand_state >> 17;
  rand_state ^= rand_state << 5;
  return rand_state;
}

void dgram_io_lwip_diag(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
