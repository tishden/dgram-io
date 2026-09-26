// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Unit tests for the record framing the stream backends put on top of TCP
// (dgram_io/stream.h). The interesting cases are all about the fact that a stream
// hands the bytes back in arbitrary chunks: a record split anywhere, several
// records in one chunk, a length prefix split across two reads, and a buffer
// that fills up before it is drained.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dgram_io/stream.h"

using namespace dgram_io;  // NOLINT: test-local convenience

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failed;
  }
}

std::vector<uint8_t> body(int i, size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t j = 0; j < len; ++j) v[j] = static_cast<uint8_t>(i * 7 + j);
  return v;
}

// Feed a framed byte stream through the deframer in chunks of `chunk` bytes
// and check every record comes back whole and in order.
void roundtrip(size_t chunk) {
  dgram_io::Framer f;
  f.init(1u << 20);
  const int n = 500;
  std::vector<std::vector<uint8_t>> sent;
  for (int i = 0; i < n; ++i) {
    const size_t len = 1 + static_cast<size_t>(i * 13 % 1400);
    sent.push_back(body(i, len));
    check(f.append(sent.back().data(), len), "append");
  }
  std::vector<uint8_t> wire(f.data(), f.data() + f.size());

  dgram_io::Deframer d;
  d.init(1400, 64 * 1024);
  size_t off = 0;
  int got = 0;
  while (off < wire.size() || d.pending() > 0) {
    d.compact();
    const size_t take = std::min(chunk, wire.size() - off);
    if (take > 0 && d.write_space() >= take) {
      std::memcpy(d.write_ptr(), wire.data() + off, take);
      d.committed(take);
      off += take;
    }
    bool bad = false;
    for (;;) {
      const uint8_t* p;
      uint32_t len;
      if (!d.next(&p, &len, &bad)) break;
      check(got < n, "too many records");
      if (got >= n) return;
      check(len == sent[got].size(), "record length");
      check(std::memcmp(p, sent[got].data(), len) == 0, "record payload");
      ++got;
    }
    check(!bad, "unexpected desync");
    if (bad) return;
    if (take == 0 && d.pending() > 0 && off >= wire.size()) break;
  }
  check(got == n, "record count for chunk " + std::to_string(chunk));
}

void oversize_is_reported() {
  dgram_io::Deframer d;
  d.init(100, 8192);
  d.compact();
  uint8_t buf[8] = {0xff, 0x0f, 1, 2, 3, 4, 5, 6};  // length 4095 > cap 100
  std::memcpy(d.write_ptr(), buf, sizeof(buf));
  d.committed(sizeof(buf));
  const uint8_t* p;
  uint32_t len;
  bool bad = false;
  check(!d.next(&p, &len, &bad), "oversize must not yield a record");
  check(bad, "oversize must set the desync flag");
}

// An empty datagram is legal on every backend; on a stream it is a bare
// prefix, and must come back as a record of length 0, not as damage.
void zero_length_roundtrip() {
  dgram_io::Framer f;
  f.init(1024);
  const uint8_t three[3] = {7, 8, 9};
  check(f.append(three, 0), "append empty");
  check(f.append(three, sizeof(three)), "append after empty");
  dgram_io::Deframer d;
  d.init(100, 8192);
  d.compact();
  std::memcpy(d.write_ptr(), f.data(), f.size());
  d.committed(f.size());
  const uint8_t* p;
  uint32_t len;
  bool bad = false;
  check(d.next(&p, &len, &bad) && len == 0, "empty record comes back empty");
  check(d.next(&p, &len, &bad) && len == 3 && p[2] == 9,
        "the record after it is intact");
  check(!bad, "empty record is not a desync");
}

void framer_refuses_when_full() {
  dgram_io::Framer f;
  f.init(1024);
  std::vector<uint8_t> rec(500, 0xab);
  check(f.append(rec.data(), rec.size()), "first fits");
  check(f.append(rec.data(), rec.size()), "second fits");
  check(!f.append(rec.data(), rec.size()), "third must be refused whole");
  check(f.size() == 2 * (500 + dgram_io::kRecPrefix), "size after refusal");
  // Partial drain then top-up: the refused record fits once space is freed.
  check(!f.fits(rec.size()), "fits() agrees with the refusal");
  f.consume(600);
  check(f.fits(rec.size()), "fits() after consume");
  check(f.append(rec.data(), rec.size()), "fits after consume");
}

}  // namespace

int main() {
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{17},
                       size_t{1400}, size_t{4096}, size_t{60000}})
    roundtrip(chunk);
  oversize_is_reported();
  zero_length_roundtrip();
  framer_refuses_when_full();
  if (g_failed) {
    fprintf(stderr, "test_stream: %d checks failed\n", g_failed);
    return 1;
  }
  printf("test_stream: OK\n");
  return 0;
}
