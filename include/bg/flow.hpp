// Bandwidth Governor - flow model and live cycle.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A flow is a unit of competing work that wants a share of scarce path
// capacity. Flow transition is an explicit, guarded state machine; invalid
// transitions fail deterministically.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/resource.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace bg {

enum class FlowState : uint32_t {
  Created = 0,
  Queued = 1,
  Reserved = 2,
  Running = 3,
  Throttled = 4,
  Preempted = 5,
  Completed = 6,
  Cancelled = 7,
  Failed = 8,
};

const char* flow_state_name(FlowState s) noexcept;

// Explicit, deterministic transition table. Nullopt means the transition is
// not permitted; callers must reject it.
std::optional<FlowState> flow_transition(FlowState from, FlowState to) noexcept;

enum class LatencyClass : uint32_t {
  LatencySensitive = 0,
  ThroughputOriented = 1,
  BestEffort = 2,
};

const char* latency_class_name(LatencyClass c) noexcept;

enum class AdmissionState : uint32_t {
  Unknown = 0,
  Admitted = 1,
  Deferred = 2,
  Rejected = 3,
};

enum class RetryState : uint32_t {
  None = 0,
  Retriable = 1,
  Exhausted = 2,
  NotRetriable = 3,
};

enum class AccountingState : uint32_t {
  Open = 0,        // not yet accounted / in flight
  Settled = 1,     // completed and released
  Released = 2,    // reservation released, accounting clean
};

// The specification of a flow as submitted by the client. Immutable once
// submitted; the coordinator may attach runtime state separately.
struct FlowSpec {
  FlowId id;
  TenantId tenant;
  WorkloadId workload;
  AttemptId attempt;
  FlowGeneration generation;
  std::string source;
  std::string destination;
  PathId path;
  uint64_t byte_count = 0;
  Directionality direction = Directionality::Unidirectional;
  int priority = 0;                 // higher = more important
  LatencyClass latency_class = LatencyClass::BestEffort;
  double deadline_seconds = 0.0;    // seconds from admission; 0 => none
  Capacity requested_min;           // minimum guarantee
  Capacity requested_preferred;     // desired rate
  Capacity requested_max;           // hard cap
  uint64_t burst_bytes = 0;         // burst budget
  bool preemptible = true;
  bool resumable = true;
};

// Advisory validation: returns a human-readable problem or nullopt if the
// spec is structurally sound (capacity ordering, bounded byte count, etc.).
std::optional<std::string> validate_flow_spec(const FlowSpec& f) noexcept;

}  // namespace bg
