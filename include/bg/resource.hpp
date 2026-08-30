// Bandwidth Governor - governed bandwidth resource model.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A resource models a scarce bandwidth capacity: a PCIe link, an NVLink-class
// peer link, host-memory bandwidth, pinned-memory transfer capacity, storage
// read/write bandwidth, inter-node TCP capacity, or a future generic transport
// backend. Every resource carries a stable identity, class, capacity state,
// generation fence, and health/readiness metadata.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"

#include <string>
#include <vector>

namespace bg {

enum class ResourceClass : uint32_t {
  Pcie = 0,
  Nvlink = 1,
  HostMemory = 2,
  PinnedMemory = 3,
  StorageRead = 4,
  StorageWrite = 5,
  InterNodeTcp = 6,
  GenericTransport = 7,
  kCount,
};

// Parse a resource class by name (case-insensitive) or return nullopt.
std::optional<ResourceClass> resource_class_from_string(std::string_view s) noexcept;

// Canonical class name (lowercase).
std::string_view resource_class_name(ResourceClass c) noexcept;

enum class Directionality : uint32_t {
  Unidirectional = 0,
  Bidirectional = 1,
};

enum class ResourceHealth : uint32_t {
  Healthy = 0,
  Degraded = 1,
  Unhealthy = 2,
};

inline const char* resource_health_name(ResourceHealth h) noexcept {
  switch (h) {
    case ResourceHealth::Healthy: return "healthy";
    case ResourceHealth::Degraded: return "degraded";
    case ResourceHealth::Unhealthy: return "unhealthy";
  }
  return "unknown";
}

// Capped resource metadata plus measured capacity report. This is the
// authoritative view a coordinator holds for one governed resource.
struct ResourceSpec {
  ResourceId id;                 // stable identity
  ResourceClass class_ = ResourceClass::GenericTransport;
  std::string source;            // topology source endpoint
  std::string destination;       // topology destination endpoint
  Directionality direction = Directionality::Unidirectional;
  Capacity nominal;              // manufacturer/measured nominal capacity
  CapacityGeneration generation; // capacity generation fence
  bool enabled = true;
};

// A complete, query-able snapshot of a resource's current state.
struct ResourceSnapshot {
  ResourceId id;
  ResourceClass class_ = ResourceClass::GenericTransport;
  std::string source;
  std::string destination;
  Directionality direction = Directionality::Unidirectional;
  Capacity nominal;
  Capacity measured;      // latest measurement
  Capacity governed;      // capacity the governor currently exposes for allocation
  Capacity reserved;      // committed to reservations
  Capacity allocated;     // currently allocated to running flows
  double instantaneous_util = 0.0;      // 0..1 utilisation now
  double moving_average_util = 0.0;     // EWMA over observation window
  uint64_t queue_depth = 0;
  bool saturated = false;
  double measured_latency_ms = -1.0;    // -1 => unavailable
  double confidence = 0.0;              // 0..1 confidence in measured capacity
  std::string provenance;
  double staleness_ms = 0.0;            // age of measurement
  double staleness_threshold_ms = 0.0;  // beyond which data is considered stale
  CapacityGeneration capacity_generation;
  ResourceHealth health = ResourceHealth::Healthy;
  bool enabled = true;
};

// Validates a measured capacity report against hard numeric bounds.
// Throws bg::value_error on impossible values. Returns the sanitised Capacity.
Capacity validate_capacity(double raw);

// Validates a measured utilisation fraction in [0,1].
double validate_utilization(double u);

}  // namespace bg
