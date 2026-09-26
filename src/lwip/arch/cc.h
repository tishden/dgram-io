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

// Diagnostics go to stderr like every other line the backends print. x is a
// parenthesised printf argument list, hence the call without parentheses.
void dgram_io_lwip_diag(const char* fmt, ...);
#define LWIP_PLATFORM_DIAG(x)   do { dgram_io_lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                             \
  do {                                                                      \
    fprintf(stderr, "lwip assert \"%s\" failed at %s:%d\n", x, __FILE__,     \
            __LINE__);                                                      \
    abort();                                                                \
  } while (0)

// Initial sequence numbers and the ARP/TCP timers' jitter, from lwip_port.c.
// Not cryptographic: ISNs from it are predictable to anyone who can see the
// traffic, which is acceptable for a stack meant for a private link.
uint32_t dgram_io_lwip_rand(void);
#define LWIP_RAND()             ((u32_t)dgram_io_lwip_rand())
