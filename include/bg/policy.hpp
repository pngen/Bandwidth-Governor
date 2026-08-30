// Bandwidth Governor - deterministic arbitration policy.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The governor implements explicit, deterministic arbitration across tenant
// fairness, workload fairness, priority, latency class, deadline pressure,
// starvation age, minimum guarantees, preferred rates, maximum caps, burst
// budgets, reservation pressure, and path bottlenecks. Decisions are fully
// explainable via Decision objects.
#pragma once

#include "bg/core.hpp"
#include "bg/explain.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bg {

// Tunable policy knobs. All values are validated as finite and bounded on use.
struct PolicyConfig {
  // Priority: weight scales as base_priority_weight^priority (priority >= 0).
  double base_priority_weight = 2.0;
  // Latency class weight multipliers.
  double latency_sensitive_weight = 4.0;
  double throughput_weight = 2.0;
  double best_effort_weight = 1.0;
  // Deadline: weight scales up to (1+deadline_weight) as pressure approaches 1.
  double deadline_weight = 1.5;
  // Deadline slack in seconds; pressure ramps over [0, slack] remaining time.
  double deadline_slack_seconds = 30.0;
  // Starvation: weight scales up to (1+starvation_weight) over starvation_slow seconds.
  double starvation_weight = 1.5;
  double starvation_slow_seconds = 60.0;
  // Fairness: how strongly under/over-served tenants are boosted/penalised.
  double fairness_gain = 1.0;
  // Reservations are honoured even at the cost of fairness (guarantees win).
  double reservation_honour = 1.0;  // 0..1
  // Admission.
  int max_retries = 3;
  bool defer_if_min_infeasible = true;  // otherwise reject
  bool reject_if_no_slack = false;
  uint32_t max_admitted = 0;  // 0 => unbounded
  // deterministic ordering for tie-breaking (0 => flow id ascending).
  bool deterministic_ties = true;
  // fairness stabilisation window for EWMA of tenant utilisation.
  double fairness_window_ms = 2000.0;
};

// A single contender used by the allocator.
struct AllocEntry {
  FlowId flow;
  double weight = 1.0;    // composite arbitration weight (see allocator)
  double min = 0.0;       // minimum guarantee (bytes/s)
  double want = 0.0;      // preferred target
  double max = 0.0;       // hard cap
};

// Result of allocating `capacity` bytes/s across `entries`. Returns the granted
// rate for each entry (never negative, never above its max, never above cap).
struct AllocResult {
  std::vector<double> granted;
  double total_requested = 0.0;   // sum of want
  double total_granted = 0.0;     // sum of granted
  double unserved = 0.0;          // total want not granted
  bool saturated = false;         // total_want > capacity
  double deadline_pressure = 0.0; // max across entries (for explanation)
};

// Compute a weighted water-fill (max-min) allocation for `capacity` capacity
// units across `entries` honouring each entry's [min,max] envelope. The result
// is deterministic and order-independent (ties broken by flow id).
AllocResult allocate_weighted(const std::vector<AllocEntry>& entries,
                              double capacity);

}  // namespace bg
