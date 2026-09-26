// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Which io_uring setups this kernel accepts, one line each. The uring backend
// fails at setup when the kernel refuses what it asks for; this says which
// flag was the one refused, without guessing from a single errno.
//
//   ./uring_probe [sqpoll-cpu]      # needs kernel.io_uring_disabled=0
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_URING
#include <liburing.h>

namespace {

void try_setup(const char* what, unsigned entries, unsigned flags,
               unsigned cq_entries, int cpu) {
  io_uring ring;
  io_uring_params p;
  std::memset(&p, 0, sizeof(p));
  p.flags = flags;
  p.cq_entries = cq_entries;
  if (cpu >= 0) p.sq_thread_cpu = static_cast<unsigned>(cpu);
  const int r = io_uring_queue_init_params(entries, &ring, &p);
  printf("%-36s %s\n", what, r == 0 ? "ok" : strerror(-r));
  if (r == 0) io_uring_queue_exit(&ring);
}

}  // namespace

int main(int argc, char** argv) {
  const int cpu = argc > 1 ? atoi(argv[1]) : 1;
  try_setup("plain", 256, 0, 0, -1);
  try_setup("CQSIZE 1024", 256, IORING_SETUP_CQSIZE, 1024, -1);
  try_setup("SQPOLL", 256, IORING_SETUP_SQPOLL, 0, -1);
  try_setup("SQPOLL + CQSIZE 1024", 256,
            IORING_SETUP_SQPOLL | IORING_SETUP_CQSIZE, 1024, -1);
  char what[64];
  snprintf(what, sizeof(what), "SQPOLL + SQ_AFF cpu %d", cpu);
  try_setup(what, 256, IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF, 0, cpu);
  snprintf(what, sizeof(what), "SQPOLL + SQ_AFF cpu %d + CQSIZE", cpu);
  try_setup(what, 256,
            IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF | IORING_SETUP_CQSIZE,
            1024, cpu);
  return 0;
}

#else

int main() {
  printf("built without io_uring support (liburing)\n");
  return 0;
}

#endif
