// Bandwidth Governor - persistence encoding.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Authoritative state is persisted as a versioned, checksummed binary blob.
// Recovery is strict: truncation, malformed lengths, duplicate fields/IDs,
// invalid enum values, invalid transitions, checksum corruption, impossible
// capacities, malformed identities, NaN/Inf, integer overflow, and trailing
// garbage are all rejected. Recovery never invents successful work.
#pragma once

#include "bg/core.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bg {

struct GovernorSnapshot;  // defined in governor.hpp

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3 polynomial), castagnoli-free, standard table-driven.
// ---------------------------------------------------------------------------
class Crc32 {
 public:
  Crc32() noexcept = default;
  void update(const uint8_t* data, size_t len) noexcept;
  uint32_t value() const noexcept { return ~crc_; }

 private:
  uint32_t crc_ = 0xFFFFFFFFu;
};

uint32_t crc32(const uint8_t* data, size_t len) noexcept;

// ---------------------------------------------------------------------------
// Binary sink (writer) with strict bounds for lengths.
// ---------------------------------------------------------------------------
class BinarySink {
 public:
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void i64(int64_t v);
  void f64(double v);
  void bytes(const uint8_t* p, size_t n);
  void str(std::string_view s);
  // 128-bit identity as two u64 words (hi then lo).
  template <class T>
  void id(const T& v) {
    u64(v.hi);
    u64(v.lo);
  }
  // Capacity as validated f64.
  void capacity(const Capacity& c) { f64(c.value()); }
  size_t size() const noexcept { return buf_.size(); }
  const std::vector<uint8_t>& data() const noexcept { return buf_; }
  void reserve(size_t n) { buf_.reserve(n); }

 private:
  std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// Binary source (reader) with strict bounds. Any read beyond the buffer throws
// bg::value_error (truncation/malformed length).
// ---------------------------------------------------------------------------
class BinarySource {
 public:
  explicit BinarySource(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  explicit BinarySource(const std::vector<uint8_t>& v)
      : p_(v.data()), end_(v.data() + v.size()) {}

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int64_t i64();
  double f64();
  void bytes(uint8_t* out, size_t n);
  std::string str();                  // reads a length-prefixed string
  std::string_view raw(size_t n);

  template <class T>
  T id() {
    uint64_t h = u64();
    uint64_t l = u64();
    return T(h, l);
  }
  Capacity capacity() { return Capacity::make(f64()); }

  size_t remaining() const noexcept { return static_cast<size_t>(end_ - p_); }
  bool empty() const noexcept { return p_ == end_; }

 private:
  void need(size_t n);
  bool in_memory_;
  const uint8_t* p_ = nullptr;
  const uint8_t* end_ = nullptr;
};

// ---------------------------------------------------------------------------
// Envelope / format versioning.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kEnvelopeMagic = "BGWOVR01";
inline constexpr uint32_t kFormatVersion = 1;

// Serialise the full authoritative snapshot into an envelope blob.
std::vector<uint8_t> encode_snapshot(const GovernorSnapshot& snap);

// Decode and validate an envelope blob into a snapshot. Throws bg::value_error
// on any corruption, truncation, or version mismatch.
GovernorSnapshot decode_snapshot(const uint8_t* data, size_t n);

}  // namespace bg
