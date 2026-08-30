// Bandwidth Governor - path model.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A path is an ordered set of resources/links a flow traverses. Paths may be
// single- or multi-link. Effective allocation for a multi-hop flow must respect
// the bottleneck resource; no allocation may violate any governed resource on
// the path. Path capacity is recalculated when resource generations change and
// stale paths are invalidated or rebuilt.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"

#include <optional>
#include <string>
#include <vector>

namespace bg {

// One hop in an ordered path. The resource generation fence is stored so the
// coordinator can detect when the hop's capacity semantics have changed.
struct PathHop {
  ResourceId resource;
  CapacityGeneration generation;
};

struct Path {
  PathId id;
  std::vector<PathHop> hops;  // ordered from source toward destination
  FlowGeneration path_generation;  // generation bump when membership changes
};

// Result of bounded path analysis.
struct PathAnalysis {
  PathId path;
  std::optional<ResourceId> bottleneck;  // nullopt => no bottleneck (empty path)
  Capacity total_requested;              // sum of requested rates of flows on path
  Capacity feasible;                     // min over hop capacities (governed)
  std::optional<ResourceId> limiting_resource;
  std::vector<FlowId> competing_flows;
  bool saturated = false;
  CapacityGeneration capacity_generation;
};

// Validation of a path's ordered membership: no duplicate resource in the
// ordered hops, hop generation non-null.
std::optional<std::string> validate_path(const Path& p) noexcept;

}  // namespace bg
