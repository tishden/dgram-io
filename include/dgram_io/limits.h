// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov

// The two datagram sizes every backend is built around.
#pragma once

#include <cstdint>

namespace dgram_io {

// How much payload one datagram carries. A default, not a constant: the
// ceiling a packet path usually hits, in a cloud especially, is counted in
// packets per second, not bytes, so how much rides in one datagram acts
// directly on the binding constraint. 1400 leaves room for IP/UDP headers
// inside the 1500-byte MTU every path carries; raise Config::max_datagram
// where the path is known to pass jumbo frames.
inline constexpr uint32_t kDefaultDatagram = 1400;

// Compile-time ceiling for that runtime value. Buffers that must hold a whole
// datagram are sized from it, and 8900 fits inside AWS's 9001-byte VPC MTU
// with room for headers. Raising it costs memory in every build, which is
// exactly why the working size stays a runtime choice.
inline constexpr uint32_t kDatagramCap = 8900;

}  // namespace dgram_io
