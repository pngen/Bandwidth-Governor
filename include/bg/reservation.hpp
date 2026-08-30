// Bandwidth Governor - reservation authority objects.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Reservations are first-class authority objects binding a flow (with its
// attempt and generation) to an ordered set of resources and allocated rates.
// Their lifecycle is transactionally guarded so that cancellation, completion,
// and failed dispatch each release exactly once; stale authority can never
// release or mutate a current reservation.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bg {

enum class ReservationState : uint32_t {
  Pending = 0,
  Active = 1,
  Released = 2,
  Cancelled = 3,
  Expired = 4,
  Failed = 5,
};

const char* reservation_state_name(ReservationState s) noexcept;

// Guarded transition table for reservations. Release is idempotent at the
// semantic level but the state machine enforces that a released reservation
// can never be released or freed again.
std::optional<ReservationState> reservation_transition(ReservationState from,
                                                       ReservationState to) noexcept;

// A single resource allocation bound to a reservation.
struct ResourceAllocation {
  ResourceId resource;
  Capacity allocated;   // bytes/second granted on this resource
};

struct Reservation {
  ReservationId id;
  FlowId flow;
  AttemptId attempt;
  FlowGeneration flow_generation;
  PathId path;
  std::vector<ResourceAllocation> allocations;  // ordered by resource id
  CoordinatorEpoch epoch;                       // coordinator authority
  WorkerBootId worker_boot;                     // issuing worker where applicable
  CapacityGeneration capacity_generation;       // capacity fence at grant time
  ReservationState state = ReservationState::Pending;
};

}  // namespace bg
