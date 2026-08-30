// Bandwidth Governor - strongly typed identity model.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Every identity used by the governor is a distinct, strongly-typed 128-bit
// value. Identity domains are never silently reused across authority
// generations; a stale identity is rejected deterministically.
#pragma once

#include "bg/core.hpp"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>

namespace bg {

// ---------------------------------------------------------------------------
// Identity tags and aliases
// ---------------------------------------------------------------------------
#define BG_ID_TAG(Name)                  \
  struct Name##Tag {                     \
    static constexpr const char* name = #Name; \
  };

BG_ID_TAG(FlowId)
BG_ID_TAG(TenantId)
BG_ID_TAG(WorkloadId)
BG_ID_TAG(LinkId)
BG_ID_TAG(PathId)
BG_ID_TAG(ReservationId)
BG_ID_TAG(AttemptId)
BG_ID_TAG(WorkerId)
BG_ID_TAG(WorkerBootId)
BG_ID_TAG(CoordinatorEpoch)
BG_ID_TAG(CapacityGeneration)
BG_ID_TAG(FlowGeneration)
BG_ID_TAG(ResourceId)
BG_ID_TAG(CoordinatorId)

#undef BG_ID_TAG

using FlowId = Id128<FlowIdTag>;
using TenantId = Id128<TenantIdTag>;
using WorkloadId = Id128<WorkloadIdTag>;
using LinkId = Id128<LinkIdTag>;
using PathId = Id128<PathIdTag>;
using ReservationId = Id128<ReservationIdTag>;
using AttemptId = Id128<AttemptIdTag>;
using WorkerId = Id128<WorkerIdTag>;
using WorkerBootId = Id128<WorkerBootIdTag>;
using CoordinatorEpoch = Id128<CoordinatorEpochTag>;
using CapacityGeneration = Id128<CapacityGenerationTag>;
using FlowGeneration = Id128<FlowGenerationTag>;
using ResourceId = Id128<ResourceIdTag>;
using CoordinatorId = Id128<CoordinatorIdTag>;

// ---------------------------------------------------------------------------
// Identity generator
// ---------------------------------------------------------------------------
// Produces unique, collision-resistant 128-bit identities. A single process
// generator uses a seeded PRNG mixed with per-object counters and a
// process-unique salt so that two coordinators never mint colliding identities.
class IdGen {
 public:
  IdGen();
  explicit IdGen(uint64_t salt);

  // Deterministic seed for reproducibility in tests.
  void seed(uint64_t salt);
  uint64_t next64() noexcept;
  template <typename T>
  T next() noexcept;

 private:
  uint64_t salt_ = 0;
  std::atomic<uint64_t> counter_{0};
  std::mt19937_64 rng_;
};

template <typename T>
inline T IdGen::next() noexcept {
  uint64_t h = rng_();
  uint64_t l = (salt_ ^ (h << 1)) ^ counter_.fetch_add(1, std::memory_order_relaxed);
  return T(h, l);
}

// ---------------------------------------------------------------------------
// Generation helpers
// ---------------------------------------------------------------------------
// A generation is 128-bit but semantically a monotonic authority marker. The
// next-generation function must preserve ordering and never wrap to a value
// that collides with an earlier generation of the same domain.
template <typename T>
inline T next_generation(const T& g) noexcept {
  uint64_t h = g.hi;
  uint64_t l = g.lo;
  if (++l == 0) ++h;  // carry; hi==0 && lo==0 (null) is never a valid generation
  l |= 1;             // never allow lo==0 to avoid colliding with the null id
  return T(h, l);
}

template <typename T>
inline T fresh_generation(uint64_t nonce) noexcept {
  return T(nonce, 1ULL);
}

}  // namespace bg
