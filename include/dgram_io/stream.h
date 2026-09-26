// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// Record framing for the stream backends (tcp, tcp-dpdk, tcp-xdp).
//
// Callers of dgram_io::Backend speak datagrams, delimited by the datagram
// boundary itself. TCP has no such boundary -- it delivers an unbroken byte
// stream, and a 1400-byte write can arrive as three reads of 500, 800 and 100
// bytes. So every datagram handed to a stream backend is wrapped in a 2-byte
// little-endian length prefix ([u16 len][record]), and the receiving side
// reassembles records out of whatever the stream hands it.
//
// The prefix is the *only* thing the stream backends add to the wire. It costs
// 2 bytes per datagram (0.14% at the 1400-byte default) and, unlike a
// self-describing header, needs no magic or version -- a mis-framed stream is
// unrecoverable either way, and TCP guarantees we never see one.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "dgram_io/backend.h"

namespace dgram_io {

inline constexpr size_t kRecPrefix = 2;

// Reassembles records out of stream chunks. Owns the buffer the records live
// in, which is what makes the Backend RX contract ("views stay valid until the
// next rx()") implementable on a stream: the leftover tail is only moved
// to the front at the *start* of the next take() round, after the caller is
// done with the previous batch.
class Deframer {
 public:
  // cap = the largest record we will ever accept; a longer length prefix
  // means the stream is desynchronised (impossible over TCP unless the peer
  // is a different build) and is reported as an error rather than papered
  // over -- a truncating reader would look exactly like packet loss.
  void init(uint32_t cap, size_t bufsize) {
    cap_ = cap;
    buf_.assign(bufsize < 4 * (cap + kRecPrefix) ? 4 * (cap + kRecPrefix)
                                                 : bufsize,
                0);
    head_ = tail_ = 0;
  }

  // Start of a round: reclaim the space the previous batch occupied. Called
  // once per rx(), before any read, so views handed out last time stay valid
  // for exactly as long as the contract promises.
  void compact() {
    if (head_ == 0) return;
    if (head_ == tail_) {
      head_ = tail_ = 0;
      return;
    }
    std::memmove(buf_.data(), buf_.data() + head_, tail_ - head_);
    tail_ -= head_;
    head_ = 0;
  }

  // Where the next stream chunk should land, and how much fits. Zero means
  // the buffer is full of undelivered records -- the caller stops reading and
  // drains what it has (back-pressure onto the TCP window, which is the point
  // of having a window at all).
  uint8_t* write_ptr() { return buf_.data() + tail_; }
  size_t write_space() const { return buf_.size() - tail_; }
  void committed(size_t n) { tail_ += n; }

  // Pops the next complete record. false = need more bytes (or desync, which
  // sets *bad).
  bool next(const uint8_t** data, uint32_t* len, bool* bad) {
    if (tail_ - head_ < kRecPrefix) return false;
    const uint8_t* p = buf_.data() + head_;
    const uint32_t rec = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
    // A zero length is a legal record: an empty datagram, which kernel UDP
    // carries too, so the stream backends must not treat it as damage.
    if (rec > cap_) {
      *bad = true;
      return false;
    }
    if (tail_ - head_ < kRecPrefix + rec) return false;
    *data = p + kRecPrefix;
    *len = rec;
    head_ += kRecPrefix + rec;
    return true;
  }

  size_t pending() const { return tail_ - head_; }

  // A whole record is buffered: next() would return one (or flag desync).
  bool has_record() const {
    if (tail_ - head_ < kRecPrefix) return false;
    const uint8_t* p = buf_.data() + head_;
    const size_t rec = static_cast<size_t>(p[0]) | (static_cast<size_t>(p[1]) << 8);
    return tail_ - head_ >= kRecPrefix + rec;
  }

 private:
  std::vector<uint8_t> buf_;
  size_t head_ = 0, tail_ = 0;
  uint32_t cap_ = 0;
};

// TX staging: framed records accumulate here and leave on flush(). A stream
// backend cannot "drop one datagram" the way a packet backend can -- half a
// record on the wire would desynchronise the peer forever -- so a record is
// either appended whole or refused whole.
class Framer {
 public:
  void init(size_t cap) {
    cap_ = cap;
    buf_.reserve(cap);
  }

  // Whether a record of `len` bytes would be accepted right now. The budget
  // is on bytes still owed to the peer, not on the vector's length: bytes
  // already handed to the socket are dead weight that a reclaim can drop.
  // Checking the vector instead would refuse records while most of the
  // buffer is stale.
  bool fits(size_t len) const { return size() + kRecPrefix + len <= cap_; }

  bool append(const void* payload, size_t len) {
    const size_t need = kRecPrefix + len;
    if (!fits(len)) return false;
    if (buf_.size() + need > cap_ && sent_ > 0) reclaim();
    const size_t off = buf_.size();
    buf_.resize(off + kRecPrefix + len);
    buf_[off] = static_cast<uint8_t>(len & 0xff);
    buf_[off + 1] = static_cast<uint8_t>(len >> 8);
    if (len) std::memcpy(buf_.data() + off + kRecPrefix, payload, len);
    return true;
  }

  const uint8_t* data() const { return buf_.data() + sent_; }
  size_t size() const { return buf_.size() - sent_; }
  bool empty() const { return sent_ == buf_.size(); }

  // Partial writes are the normal case on a socket that is not allowed to
  // block: consume what left and keep the rest for the next flush.
  void consume(size_t n) {
    sent_ += n;
    if (sent_ == buf_.size()) {
      buf_.clear();
      sent_ = 0;
    } else if (sent_ > buf_.size() / 2 && sent_ > 64 * 1024) {
      reclaim();
    }
  }

 private:
  void reclaim() {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(sent_));
    sent_ = 0;
  }

  std::vector<uint8_t> buf_;
  size_t sent_ = 0, cap_ = 0;
};

}  // namespace dgram_io
