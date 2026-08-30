// Bandwidth Governor - explainability model.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Every important admission, scheduling, throttling, preemption, rejection, and
// allocation decision carries a structured explanation. We never rely on an
// opaque scalar score without exposing the constituent decision factors.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/resource.hpp"
#include "bg/flow.hpp"

#include <optional>
#include <string>
#include <vector>

namespace bg {

enum class DecisionKind : uint32_t {
  Admit = 0,
  Defer = 1,
  Reject = 2,
  Throttle = 3,
  Preempt = 4,
  Grant = 5,     // a reservation grant
  Release = 6,   // a reservation release
};

const char* decision_kind_name(DecisionKind k) noexcept;

// A named decision factor. value is a nominal numeric contribution (possibly
// negative); weight is the policy weight applied; rationale is the human
// explanation. These are combined only after each is surfaced.
struct DecisionFactor {
  std::string name;
  double value = 0.0;
  double weight = 0.0;
  std::string rationale;  // "why" for this factor
};

struct Decision {
  FlowId flow;
  TenantId tenant;
  WorkloadId workload;
  DecisionKind kind = DecisionKind::Defer;
  Capacity requested;
  Capacity granted;
  Capacity minimum_guarantee;
  Capacity preferred;
  Capacity maximum;
  std::optional<ResourceId> bottleneck;  // nullopt => no resource-limited bottleneck
  PathId path;
  int priority = 0;
  LatencyClass latency_class = LatencyClass::BestEffort;
  double deadline_pressure = 0.0;    // 0..1 rising pressure
  double tenant_fairness = 0.0;      // current fairness share for tenant
  double workload_fairness = 0.0;    // current fairness share for workload
  double starvation_age = 0.0;       // seconds waiting
  uint64_t burst_remaining = 0;
  bool saturated = false;
  std::vector<FlowId> competing_flows;
  double reservation_pressure = 0.0; // 0..1
  CapacityGeneration resource_generation;
  CapacityGeneration capacity_generation;
  std::string kind_reason;           // high-level reason for the decision
  std::string defer_reason;          // why a flow was deferred
  std::string reject_reason;         // why a flow was rejected
  std::string throttle_reason;       // why a flow was throttled
  std::string preemption_reason;     // why a flow was preempted
  std::vector<DecisionFactor> factors;
};

}  // namespace bg
