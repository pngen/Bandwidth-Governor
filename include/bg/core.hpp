// Bandwidth Governor - core primitives.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Core value types, validation, and identity primitives shared across the
// Bandwidth Governor runtime. This header is intentionally lean and
// self-contained so that it can be included by every translation unit.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
namespace bg {

inline constexpr int kMajor = 1;
inline constexpr int kMinor = 0;
inline constexpr int kPatch = 0;
inline constexpr const char* kVersion = "1.0.0";
inline constexpr const char* kVersionFull = "1.0.0";
inline constexpr uint32_t kStateFormatVersion = 1;  // binary state format

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
// Policy/correctness error: a caller attempted an operation that the governor
// rejects deterministically (invalid transition, stale authority, bad value...).
class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A structurally invalid value (NaN, negative capacity, overflow...).
class value_error : public error {
 public:
  using error::error;
};

// A stale/foreign authority was presented and was rejected.
class authority_error : public error {
 public:
  using error::error;
};

// An invalid lifecycle/state transition was attempted.
class transition_error : public error {
 public:
  using error::error;
};

// ---------------------------------------------------------------------------
// Numeric helpers
// ---------------------------------------------------------------------------
constexpr double kMaxRate = 1.0e18;       // hard upper bound on any rate (bytes/s)
constexpr double kMaxBytes = 1.0e18;      // hard upper bound on any byte count
constexpr double kEpsilon = 1.0e-9;

inline double clamp(double v, double lo, double hi) noexcept {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

inline bool finite_positive(double v) noexcept {
  return v > 0.0 && v < kMaxRate;
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------
// A validated, bounded capacity value measured in bytes/second. All capacity
// values flowing through the governor must be constructed through Capacity so
// that NaN, Inf, negative and impossible rates are rejected deterministically.
class Capacity {
 public:
  // Default capacity is a valid zero allocation.
  constexpr Capacity() noexcept = default;

  // Construct from a raw rate. Throws bg::value_error if invalid.
  static Capacity make(double v) {
    Capacity c;
    c.assign(v);  // throws if invalid
    return c;
  }

  // Construct from a raw rate; returns nullopt if invalid.
  static std::optional<Capacity> try_make(double v) noexcept {
    if (!is_valid_rate(v)) return std::nullopt;
    Capacity c;
    c.value_ = v;
    return c;
  }

  // A default-constructed Capacity represents "no allocation" (0 bytes/s).
  constexpr double value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0.0; }
  constexpr bool valid() const noexcept { return true; }  // invariant-held

  // Assigned only when the value has been validated.
  constexpr void set_value(double v) noexcept {
    value_ = (is_valid_rate(v)) ? v : 0.0;
  }

  static constexpr bool is_valid_rate(double v) noexcept {
    return !(v < 0.0) && !(v > kMaxRate) && !(v != v) && !(v == std::numeric_limits<double>::infinity());
  }

  friend constexpr bool operator==(const Capacity& a, const Capacity& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Capacity& a, const Capacity& b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(const Capacity& a, const Capacity& b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Capacity& a, const Capacity& b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(const Capacity& a, const Capacity& b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator>=(const Capacity& a, const Capacity& b) noexcept {
    return a.value_ >= b.value_;
  }

  Capacity& operator+=(const Capacity& other) {
    set_value(value_ + other.value_);
    return *this;
  }
  Capacity& operator-=(const Capacity& other) {
    set_value(value_ - other.value_);
    return *this;
  }

 private:
  void assign(double v) {
    if (!is_valid_rate(v)) {
      throw value_error("capacity value invalid: negative, NaN, Inf, or above bound");
    }
    value_ = v;
  }
  double value_ = 0.0;
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint now() noexcept { return Clock::now(); }
inline double now_ms() noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
inline double seconds_between(TimePoint a, TimePoint b) noexcept {
  return std::chrono::duration<double>(b - a).count();
}
inline double ms_between(TimePoint a, TimePoint b) noexcept {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---------------------------------------------------------------------------
// Fixed 128-bit identity
// ---------------------------------------------------------------------------
namespace detail {

// Two-64-bit-word identity backing store. Provides the value semantics and
// comparison used by every strongly-typed identity. Kept trivial: identities
// are value types and must remain trivially copyable and equality-consistent.
struct Id128Base {
  uint64_t hi = 0;
  uint64_t lo = 0;

  constexpr Id128Base() noexcept = default;
  constexpr Id128Base(uint64_t h, uint64_t l) noexcept : hi(h), lo(l) {}

  constexpr bool is_null() const noexcept { return hi == 0 && lo == 0; }
  constexpr bool equals(const Id128Base& o) const noexcept {
    return hi == o.hi && lo == o.lo;
  }
};

// Format helper: hex encode the two 64-bit words into a 32-char string.
inline void append_hex16(std::string& out, uint64_t v) {
  char buf[16];
  static const char* hex = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    buf[i] = hex[v & 0xF];
    v >>= 4;
  }
  out.append(buf, 16);
}

}  // namespace detail

// Strongly-typed 128-bit identity. Each distinct identity domain is a distinct
// C++ type so that mixing FlowId with TenantId is impossible at compile time.
template <typename Tag>
class Id128 : public detail::Id128Base {
 public:
  using detail::Id128Base::Id128Base;
  constexpr Id128() noexcept = default;

  // Domain name for diagnostics and serialization.
  static constexpr std::string_view type_name() noexcept { return Tag::name; }

  constexpr bool operator==(const Id128& o) const noexcept {
    return detail::Id128Base::equals(o);
  }
  constexpr bool operator!=(const Id128& o) const noexcept { return !(*this == o); }
  constexpr bool operator<(const Id128& o) const noexcept {
    if (hi != o.hi) return hi < o.hi;
    return lo < o.lo;
  }
  constexpr bool operator>(const Id128& o) const noexcept { return o < *this; }
  constexpr bool operator<=(const Id128& o) const noexcept { return !(o < *this); }
  constexpr bool operator>=(const Id128& o) const noexcept { return !(*this < o); }

  // Format as 32 hex digits, big-endian word order (hi first).
  std::string to_string() const {
    std::string s;
    s.reserve(32);
    detail::append_hex16(s, hi);
    detail::append_hex16(s, lo);
    return s;
  }

  static constexpr std::size_t kHexLen = 32;
};

}  // namespace bg

// ---- Generation semantics -------------------------------------------------
// Generation identities are bumped monotonically on every authority change.
// The bump helper lives in ids.hpp (needs Id128 operators). Placeholder for
// the hash specialization.

namespace std {
template <typename Tag>
struct hash<bg::Id128<Tag>> {
  size_t operator()(const bg::Id128<Tag>& id) const noexcept {
    uint64_t h = id.hi ^ (id.lo * 0x9E3779B97F4A7C15ULL);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return static_cast<size_t>(h);
  }
};
}  // namespace std
