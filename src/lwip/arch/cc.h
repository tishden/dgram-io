// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// lwIP platform header for a plain Linux userspace process. lwIP expects the
// port to supply this; on a hosted C library almost everything it asks about
// already has a standard answer, so this file is mostly "use libc".
#pragma once

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define LWIP_ERRNO_STDINCLUDE   1
#define LWIP_NO_UNISTD_H        0
#define LWIP_TIMEVAL_PRIVATE    0

#if BYTE_ORDER == BIG_ENDIAN
#define LWIP_PLATFORM_BYTESWAP  0
#endif

// Diagnostics go to stderr like every other line this transport prints, so a
// stack complaint lands in the same .sender.log the bench driver collects.
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                             \
  do {                                                                      \
    fprintf(stderr, "lwip assert \"%s\" failed at %s:%d\n", x, __FILE__,     \
            __LINE__);                                                      \
    abort();                                                                \
  } while (0)

// Initial sequence numbers and the ARP/TCP timers' jitter. Seeded in
// lwip_port.c; nothing here is security-relevant on a point-to-point DAC.
#define LWIP_RAND()             ((u32_t)random())
