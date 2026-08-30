// Bandwidth Governor - core identity implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/core.hpp"
#include "bg/ids.hpp"

#include <cstdint>

namespace bg {

IdGen::IdGen() { seed(0); }

IdGen::IdGen(uint64_t salt) { seed(salt); }

void IdGen::seed(uint64_t salt) {
  salt_ = salt;
  counter_.store(0, std::memory_order_relaxed);
  // Derive an unpredictable seed when the caller supplied 0.
  uint64_t seed_value = salt;
  if (seed_value == 0) {
    seed_value = (static_cast<uint64_t>(now_ms() * 1000.0)) ^
                 reinterpret_cast<std::uintptr_t>(this);
  }
  rng_.seed(seed_value);
}

uint64_t IdGen::next64() noexcept {
  uint64_t h = rng_();
  uint64_t l = (salt_ ^ (h << 1)) ^ counter_.fetch_add(1, std::memory_order_relaxed);
  return (h << 32) ^ (l & 0xFFFFFFFFULL);
}

}  // namespace bg
