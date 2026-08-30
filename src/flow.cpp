// Bandworth Governor - flow implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/flow.hpp"

namespace bg {

const char* flow_state_name(FlowState s) noexcept {
  switch (s) {
    case FlowState::Created: return "created";
    case FlowState::Queued: return "queued";
    case FlowState::Reserved: return "reserved";
    case FlowState::Running: return "running";
    case FlowState::Throttled: return "throttled";
    case FlowState::Preempted: return "preempted";
    case FlowState::Completed: return "completed";
    case FlowState::Cancelled: return "cancelled";
    case FlowState::Failed: return "failed";
  }
  return "unknown";
}

std::optional<FlowState> flow_transition(FlowState from, FlowState to) noexcept {
  if (from == to) return to;  // reflexive transition is a no-op
  switch (from) {
    case FlowState::Created:
      if (to == FlowState::Queued || to == FlowState::Cancelled ||
          to == FlowState::Failed)
        return to;
      break;
    case FlowState::Queued:
      if (to == FlowState::Reserved || to == FlowState::Preempted ||
          to == FlowState::Cancelled || to == FlowState::Failed ||
          to == FlowState::Running)
        return to;
      break;
    case FlowState::Reserved:
      if (to == FlowState::Running || to == FlowState::Preempted ||
          to == FlowState::Cancelled || to == FlowState::Failed ||
          to == FlowState::Completed)
        return to;
      break;
    case FlowState::Running:
      if (to == FlowState::Throttled || to == FlowState::Preempted ||
          to == FlowState::Completed || to == FlowState::Cancelled ||
          to == FlowState::Failed || to == FlowState::Reserved)
        return to;
      break;
    case FlowState::Throttled:
      if (to == FlowState::Running || to == FlowState::Preempted ||
          to == FlowState::Completed || to == FlowState::Cancelled ||
          to == FlowState::Failed || to == FlowState::Reserved)
        return to;
      break;
    case FlowState::Preempted:
      if (to == FlowState::Queued || to == FlowState::Running ||
          to == FlowState::Cancelled || to == FlowState::Failed ||
          to == FlowState::Reserved)
        return to;
      break;
    case FlowState::Completed:
    case FlowState::Cancelled:
    case FlowState::Failed:
      // terminal: no outbound transitions
      break;
  }
  return std::nullopt;
}

const char* latency_class_name(LatencyClass c) noexcept {
  switch (c) {
    case LatencyClass::LatencySensitive: return "latency_sensitive";
    case LatencyClass::ThroughputOriented: return "throughput_oriented";
    case LatencyClass::BestEffort: return "best_effort";
  }
  return "unknown";
}

std::optional<std::string> validate_flow_spec(const FlowSpec& f) noexcept {
  if (f.id.is_null()) return "flow id is null";
  if (f.tenant.is_null()) return "tenant id is null";
  if (f.attempt.is_null()) return "attempt id is null";
  if (f.generation.is_null()) return "flow generation is null (flow must be generation fenced)";
  if (f.byte_count == 0) return "byte_count must be positive";
  if (f.byte_count > static_cast<uint64_t>(kMaxBytes))
    return "byte_count exceeds the absolute bound";
  if (f.requested_max.value() < f.requested_min.value())
    return "requested_max is below requested_min";
  if (f.requested_preferred.value() < f.requested_min.value())
    return "requested_preferred is below requested_min";
  if (f.requested_preferred.value() > f.requested_max.value())
    return "requested_preferred exceeds requested_max";
  if (f.deadline_seconds < 0.0) return "deadline_seconds must be non-negative";
  return std::nullopt;
}

}  // namespace bg
