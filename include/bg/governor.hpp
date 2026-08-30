// Bandwidth Governor - coordinator core.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The Governor is the authoritative coordinator: it owns flow and reservation
// state, arbitrates scarce capacity, honours reservations, fences stale
// authority, and persists its authoritative state. It is thread-safe: all
// public methods are serialised, and no blocking network, persistence, or
// backend operation is ever performed while the governor lock is held.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/resource.hpp"
#include "bg/flow.hpp"
#include "bg/path.hpp"
#include "bg/policy.hpp"
#include "bg/explain.hpp"
#include "bg/reservation.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bg {

struct GovernorConfig {
  PolicyConfig policy;
  uint64_t id_salt = 0;          // 0 => derive from process entropy
  bool auto_clock = true;        // false => tests drive time via advance_ms
};

// Worker registration payload used by the distributed authority.
struct WorkerRegistration {
  WorkerId worker;
  WorkerBootId boot;
  std::vector<ResourceSpec> inventory;  // resources this backend backs
  uint32_t protocol_version = 1;
  std::string backend;                  // e.g. synthetic, cuda
};

// Admission outcome returned to a submitter.
struct AdmissionResult {
  bool admitted = false;
  FlowId flow;
  Decision decision;
  std::optional<Reservation> reservation;  // present when reserved
};

// Public flow snapshot combining spec + live runtime state.
struct FlowSnapshot {
  FlowSpec spec;
  FlowState state = FlowState::Created;
  Capacity granted;              // current governed grant (bytes/s)
  double admitted_ms = 0.0;
  double last_update_ms = 0.0;
  uint64_t bytes_transferred = 0;
  uint64_t burst_remaining = 0;
  uint64_t retry_count = 0;
  double starvation_ms = 0.0;
  std::optional<ReservationId> reservation;
  std::optional<WorkerId> assigned_worker;   // worker driving this flow (if any)
  WorkerBootId assigned_boot;                // that worker's boot authority
  std::string last_error;
};

// Persistent tenant / workload accounting.
struct TenantAccounting {
  TenantId tenant;
  double allocated_now = 0.0;
  double served_bytes = 0.0;
  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t failed = 0;
  double weight = 1.0;
};

struct WorkloadAccounting {
  WorkloadId workload;
  double allocated_now = 0.0;
  double served_bytes = 0.0;
  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t failed = 0;
  double weight = 1.0;
};

// Full authoritative snapshot, the unit of persistence and restoration.
struct GovernorSnapshot {
  uint32_t format_version = kStateFormatVersion;
  CoordinatorId coordinator;
  CoordinatorEpoch epoch;
  CapacityGeneration capacity_generation;
  PolicyConfig policy;
  std::vector<ResourceSpec> resources;
  std::vector<Path> paths;
  std::vector<FlowSnapshot> flows;
  std::vector<Reservation> reservations;
  std::map<TenantId, TenantAccounting> tenants;
  std::map<WorkloadId, WorkloadAccounting> workloads;
  std::unordered_map<ResourceId, ResourceSnapshot> resource_state;
  double saved_at_ms = 0.0;
};

class Governor {
 public:
  explicit Governor(GovernorConfig cfg = {});
  ~Governor();
  Governor(const Governor&) = delete;
  Governor& operator=(const Governor&) = delete;
  Governor(Governor&&) = delete;
  Governor& operator=(Governor&&) = delete;

  // --- clock control (for deterministic tests) ---
  void set_auto_clock(bool auto_c);
  void advance_ms(double dt);
  double now_ms() const;

  // --- resources ---
  ResourceId add_resource(const ResourceSpec& spec);
  void update_capacity(const ResourceId& res, double measured, double confidence,
                      std::string provenance, double measured_latency_ms = -1.0,
                      uint64_t queue_depth = 0, bool bump_generation = false);
  void set_resource_enabled(const ResourceId& res, bool enabled);
  void invalidate_resource_capacity(const ResourceId& res);  // bump generation
  std::vector<ResourceSnapshot> list_resources() const;
  std::optional<ResourceSnapshot> resource(const ResourceId& res) const;

  // --- paths ---
  PathId add_path(const Path& path);
  std::vector<Path> list_paths() const;
  std::optional<PathAnalysis> analyze_path(const PathId& path) const;
  void invalidate_path(const PathId& path);

  // --- flows ---
  AdmissionResult submit_flow(const FlowSpec& spec);
  bool cancel_flow(const FlowId& flow, std::string reason);
  bool resume_flow(const FlowId& flow);
  void tick();   // recompute allocations, enforce preemption/throttle
  std::vector<FlowSnapshot> list_flows() const;
  std::optional<FlowSnapshot> flow(const FlowId& f) const;
  Decision explain_flow(const FlowId& f) const;
  bool explain_admission(const FlowSpec& spec, Decision& out) const;

  // --- reservations ---
  std::vector<Reservation> list_reservations() const;
  std::optional<Reservation> reservation(const ReservationId& id) const;
  bool release_reservation(const ReservationId& id);

  // --- distributed authority ---
  WorkerId register_worker(const WorkerRegistration& reg);
  void deregister_worker(const WorkerId& w);
  CoordinatorId coordinator() const;
  CoordinatorEpoch epoch() const;
  CapacityGeneration capacity_generation() const;
  bool report_progress(const FlowId& flow, const AttemptId& attempt,
                       const FlowGeneration& fgen, const WorkerBootId& boot,
                       uint64_t bytes_transferred);
  bool report_completion(const FlowId& flow, const AttemptId& attempt,
                         const FlowGeneration& fgen, const WorkerBootId& boot,
                         uint64_t bytes_transferred);

  // --- snapshot / persistence ---
  GovernorSnapshot snapshot() const;
  void restore(const GovernorSnapshot& snap);
  std::vector<uint8_t> save() const;      // encoded envelope
  void load(const uint8_t* data, size_t n);
  void save_file(const std::string& path) const;   // atomic file write
  void load_file(const std::string& path);

  // --- accounting / saturation diagnostics ---
  std::map<TenantId, double> tenant_allocations() const;
  std::map<WorkloadId, double> workload_allocations() const;
  bool accounting_at_zero() const;  // all allocated/reserved sums == 0

 private:
  struct Impl;
  GovernorSnapshot build_snapshot_locked() const;  // caller must hold the lock
  std::unique_ptr<Impl> impl_;
};

}  // namespace bg
