// Bandwidth Governor - coordinator core implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/governor.hpp"

#include "bg/persist.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>

namespace bg {

namespace {
constexpr double kEps = 1e-9;

double governed_of(const ResourceSpec& spec, double measured, bool healthy,
                   bool enabled, double freshness) {
  if (!enabled || !healthy) return 0.0;
  double nominal = spec.nominal.value();
  double applied = measured > 0.0 ? measured : nominal;
  applied *= freshness;
  if (applied < 0.0 || applied > kMaxRate) applied = nominal;
  return applied;
}

double freshness_of(double staleness_ms, double threshold_ms) {
  if (threshold_ms <= 0.0) return 1.0;
  double ratio = staleness_ms / threshold_ms;
  if (ratio <= 0.0) return 1.0;
  if (ratio >= 1.0) return 0.0;
  return 1.0 - ratio;
}

// Build an admission/reject/defer decision with the common factor set.
Decision make_decision(const FlowSpec& spec, DecisionKind kind, double granted,
                       double bottleneck_cap, bool saturated, std::string reason,
                       std::optional<ResourceId> bottleneck) {
  Decision d;
  d.flow = spec.id;
  d.tenant = spec.tenant;
  d.workload = spec.workload;
  d.kind = kind;
  d.requested = spec.requested_preferred;
  d.granted = Capacity::try_make(granted).value_or(Capacity{});
  d.minimum_guarantee = spec.requested_min;
  d.preferred = spec.requested_preferred;
  d.maximum = spec.requested_max;
  d.bottleneck = bottleneck;
  d.path = spec.path;
  d.priority = spec.priority;
  d.latency_class = spec.latency_class;
  d.saturated = saturated;
  d.kind_reason = std::move(reason);
  (void)bottleneck_cap;
  return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// Implementation state
// ---------------------------------------------------------------------------
struct WorkerRT {
  WorkerId id;
  WorkerBootId boot;
  std::string backend;
  uint32_t protocol = 1;
  bool alive = true;
  std::vector<ResourceId> back_resources;
};

struct ResRT {
  ResourceSpec spec;
  double measured = 0.0;
  double moving_avg = 0.0;
  double inst_util = 0.0;
  double last_ms = 0.0;
  double confidence = 0.0;
  double latency_ms = -1.0;
  uint64_t qdepth = 0;
  bool saturated = false;
  ResourceHealth health = ResourceHealth::Healthy;
  bool enabled = true;
  double reserved = 0.0;
  double allocated = 0.0;
  double governed = 0.0;
  std::string provenance;
};

struct FlowRT {
  FlowSpec spec;
  FlowState state = FlowState::Created;
  double admitted_ms = 0.0;
  double last_ms = 0.0;
  double starvation_ms = 0.0;
  double granted = 0.0;
  uint64_t bytes_done = 0;
  uint64_t burst_remaining = 0;
  uint64_t retries = 0;
  std::optional<ReservationId> reservation;
  std::optional<WorkerId> assigned_worker;
  WorkerBootId assigned_boot;
  std::string last_error;
  double last_grant_update_ms = 0.0;
};

struct TenantAcc {
  double allocated_now = 0.0;
  double served_bytes = 0.0;
  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t failed = 0;
  double weight = 1.0;
};

struct WorkloadAcc {
  double allocated_now = 0.0;
  double served_bytes = 0.0;
  uint64_t submitted = 0;
  uint64_t completed = 0;
  uint64_t failed = 0;
  double weight = 1.0;
};

struct Governor::Impl {
  explicit Impl(GovernorConfig c) : config(std::move(c)) {
    if (config.id_salt != 0) gen.seed(config.id_salt);
    coordinator = gen.next<CoordinatorId>();
    epoch = fresh_generation<CoordinatorEpoch>(gen.next64());
    capacity_generation = fresh_generation<CapacityGeneration>(gen.next64());
  }

  GovernorConfig config;
  IdGen gen;
  CoordinatorId coordinator;
  CoordinatorEpoch epoch;
  CapacityGeneration capacity_generation;
  bool auto_clock = true;
  double virtual_now = 0.0;
  mutable std::mutex mutex;

  std::unordered_map<ResourceId, ResRT> resources;
  std::unordered_map<PathId, Path> paths;
  std::unordered_map<FlowId, FlowRT> flows;
  std::unordered_map<ReservationId, Reservation> reservations;
  std::unordered_map<WorkerId, WorkerRT> workers;
  std::unordered_map<TenantId, TenantAcc> tenants;
  std::unordered_map<WorkloadId, WorkloadAcc> workloads;

  double now() const { return auto_clock ? bg::now_ms() : virtual_now; }

  double tenant_weight(const TenantAcc& t) const { return t.weight <= 0.0 ? 1.0 : t.weight; }

  double flow_weight(const FlowRT& f, double now_ms) const {
    const PolicyConfig& p = config.policy;
    double prio = static_cast<double>(std::max(0, f.spec.priority));
    double w = std::pow(p.base_priority_weight, prio);
    switch (f.spec.latency_class) {
      case LatencyClass::LatencySensitive: w *= p.latency_sensitive_weight; break;
      case LatencyClass::ThroughputOriented: w *= p.throughput_weight; break;
      case LatencyClass::BestEffort: w *= p.best_effort_weight; break;
    }
    if (f.spec.deadline_seconds > 0.0) {
      double deadline_ms = f.spec.deadline_seconds * 1000.0;
      double elapsed = now_ms - f.admitted_ms;
      double remaining = deadline_ms - elapsed;
      double pressure = remaining <= 0.0 ? 1.0
                                         : (1.0 - remaining / (p.deadline_slack_seconds * 1000.0));
      pressure = std::clamp(pressure, 0.0, 1.0);
      w *= (1.0 + p.deadline_weight * pressure);
    }
    double starve = std::min(1.0, f.starvation_ms / (p.starvation_slow_seconds * 1000.0));
    w *= (1.0 + p.starvation_weight * std::max(0.0, starve));
    auto tit = tenants.find(f.spec.tenant);
    if (tit != tenants.end()) {
      double total_w = 0.0;
      for (const auto& [tid, ta] : tenants) { (void)tid; total_w += tenant_weight(ta); }
      double ideal = (total_w > 0.0) ? (tenant_weight(tit->second) / total_w) : 1.0;
      double total_served = 0.0;
      for (const auto& [tid, ta] : tenants) { (void)tid; total_served += ta.served_bytes; }
      double actual = (total_served > 0.0) ? (tit->second.served_bytes / total_served) : 0.0;
      double diff = ideal - actual;
      w *= std::clamp(1.0 + p.fairness_gain * diff, 0.25, 4.0);
    }
    if (f.burst_remaining > 0) w *= 1.1;
    return w;
  }

  void set_flow_state(FlowRT& f, FlowState to) {
    auto next = flow_transition(f.state, to);
    if (!next) {
      throw transition_error(std::string("invalid flow transition: ") +
                             flow_state_name(f.state) + " -> " + flow_state_name(to));
    }
    f.state = *next;
  }

  void recompute_governed() {
    for (auto& [id, r] : resources) {
      (void)id;
      double staleness = (r.last_ms == 0.0) ? 0.0 : (now() - r.last_ms);
      double freshness = freshness_of(staleness, config.policy.fairness_window_ms);
      r.governed = governed_of(r.spec, r.measured, r.health == ResourceHealth::Healthy,
                               r.enabled, freshness);
    }
  }

  void recompute_reserved() {
    for (auto& [id, r] : resources) { (void)id; r.reserved = 0.0; }
    for (const auto& [rid, rsv] : reservations) {
      (void)rid;
      if (rsv.state != ReservationState::Active && rsv.state != ReservationState::Pending)
        continue;
      auto fit = flows.find(rsv.flow);
      if (fit == flows.end()) continue;
      FlowState fs = fit->second.state;
      if (fs == FlowState::Running || fs == FlowState::Throttled || fs == FlowState::Reserved)
        continue;  // scheduled -> accounted via allocated
      for (const auto& al : rsv.allocations) {
        auto rit = resources.find(al.resource);
        if (rit != resources.end()) rit->second.reserved += al.allocated.value();
      }
    }
  }

  void recompute_allocated() {
    for (auto& [id, r] : resources) { (void)id; r.allocated = 0.0; }
    for (auto& [fid, f] : flows) {
      (void)fid;
      if (f.state != FlowState::Running && f.state != FlowState::Throttled) continue;
      if (f.granted <= 0.0) continue;
      auto pit = paths.find(f.spec.path);
      if (pit == paths.end()) continue;
      for (const auto& hop : pit->second.hops) {
        auto rit = resources.find(hop.resource);
        if (rit != resources.end()) rit->second.allocated += f.granted;
      }
    }
  }

  void recompute_saturation() {
    for (auto& [id, r] : resources) {
      (void)id;
      r.saturated = (r.allocated + r.reserved) >= r.governed * (1.0 - kEps);
    }
  }

  void refresh_diagnostics() {
    recompute_governed();
    recompute_reserved();
    recompute_allocated();
    recompute_saturation();
  }

  void advance_tenant_starvation(double now_ms) {
    for (auto& [fid, f] : flows) {
      (void)fid;
      if (f.state == FlowState::Running) continue;
      double elapsed = now_ms - f.last_ms;
      if (elapsed > 0.0) f.starvation_ms += elapsed;
      f.last_ms = now_ms;
    }
  }

  // Release a reservation exactly once. Caller must hold the lock.
  void release_reservation_internal(const ReservationId& rid) {
    auto it = reservations.find(rid);
    if (it == reservations.end()) return;
    Reservation& rsv = it->second;
    auto next = reservation_transition(rsv.state, ReservationState::Released);
    if (!next) {
      // already terminal (released/cancelled/expired/failed): exactly-once.
      return;
    }
    rsv.state = *next;
    // The reservation's capacities are no longer reserved; recompute done by caller.
  }

  void cancel_reservation_internal(const ReservationId& rid) {
    auto it = reservations.find(rid);
    if (it == reservations.end()) return;
    Reservation& rsv = it->second;
    auto next = reservation_transition(rsv.state, ReservationState::Cancelled);
    if (next) rsv.state = *next;
  }

  // Pick an alive worker that backs any hop of the path (prefers the bottleneck).
  std::unordered_map<WorkerId, WorkerRT>::iterator pick_worker_for_path(const PathId& pid) {
    auto pit = paths.find(pid);
    if (pit == paths.end()) return workers.end();
    for (auto& [wid, w] : workers) {
      if (!w.alive) continue;
      for (const PathHop& hop : pit->second.hops) {
        for (const ResourceId& rid : w.back_resources) {
          if (rid == hop.resource) return workers.find(wid);
        }
      }
    }
    return workers.end();
  }

  // Attempt to admit a queued (deferred) flow into a reservation.
  void admit_queued(FlowRT& f) {
    auto pit = paths.find(f.spec.path);
    if (pit == paths.end()) return;
    double bottleneck_avail = kMaxRate;
    bool feasible = true;
    for (const PathHop& hop : pit->second.hops) {
      auto rit = resources.find(hop.resource);
      if (rit == resources.end()) { feasible = false; break; }
      double avail = rit->second.governed - rit->second.reserved - rit->second.allocated;
      if (avail < 0.0) avail = 0.0;
      if (avail + kEps < f.spec.requested_min.value()) feasible = false;
      if (avail < bottleneck_avail) bottleneck_avail = avail;
    }
    if (!feasible || bottleneck_avail < f.spec.requested_min.value() - kEps) return;
    double grant = std::min({f.spec.requested_preferred.value(), f.spec.requested_max.value(),
                             bottleneck_avail});
    if (grant < f.spec.requested_min.value()) grant = f.spec.requested_min.value();
    grant = std::clamp(grant, f.spec.requested_min.value(), f.spec.requested_max.value());
    Reservation rsv;
    rsv.id = gen.next<ReservationId>();
    rsv.flow = f.spec.id;
    rsv.attempt = f.spec.attempt;
    rsv.flow_generation = f.spec.generation;
    rsv.path = f.spec.path;
    rsv.epoch = epoch;
    rsv.capacity_generation = capacity_generation;
    rsv.state = ReservationState::Active;
    for (const PathHop& hop : pit->second.hops) {
      ResourceAllocation al;
      al.resource = hop.resource;
      al.allocated = Capacity::make(grant);
      rsv.allocations.push_back(al);
    }
    reservations[rsv.id] = rsv;
    f.reservation = rsv.id;
    f.granted = grant;
    f.admitted_ms = now();
    set_flow_state(f, FlowState::Reserved);
  }
};

// ---- construction / accessors -------------------------------------------------
Governor::Governor(GovernorConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Governor::~Governor() = default;

CoordinatorId Governor::coordinator() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->coordinator;
}

CoordinatorEpoch Governor::epoch() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->epoch;
}

CapacityGeneration Governor::capacity_generation() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->capacity_generation;
}

void Governor::set_auto_clock(bool auto_c) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  impl_->auto_clock = auto_c;
}

void Governor::advance_ms(double dt) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  impl_->virtual_now += dt;
}

double Governor::now_ms() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->now();
}

// ---- resources ----------------------------------------------------------------
ResourceId Governor::add_resource(const ResourceSpec& spec) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto& r = impl_->resources[spec.id];
  r.spec = spec;
  if (spec.generation.is_null()) {
    // assign a fresh generation if the caller did not fence the resource.
    r.spec.generation = fresh_generation<CapacityGeneration>(impl_->gen.next64());
  }
  r.measured = 0.0;
  r.last_ms = impl_->now();
  r.confidence = 0.0;
  impl_->recompute_governed();
  return spec.id;
}

void Governor::update_capacity(const ResourceId& res, double measured, double confidence,
                               std::string provenance, double measured_latency_ms,
                               uint64_t queue_depth, bool bump_generation) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->resources.find(res);
  if (it == impl_->resources.end()) return;
  ResRT& r = it->second;
  if (!Capacity::is_valid_rate(measured) || !(confidence >= 0.0 && confidence <= 1.0)) {
    throw value_error("invalid capacity update: bad measured rate or confidence");
  }
  r.measured = measured;
  r.confidence = confidence;
  r.provenance = std::move(provenance);
  r.latency_ms = measured_latency_ms;
  r.qdepth = queue_depth;
  // EWMA of moving average utilisation, weighted by window fraction.
  r.moving_avg = r.moving_avg * 0.7 + (measured / (r.spec.nominal.value() > 0.0
                                                   ? r.spec.nominal.value() : 1.0)) * 0.3;
  r.last_ms = impl_->now();
  if (bump_generation) {
    r.spec.generation = next_generation(r.spec.generation);
    impl_->capacity_generation = next_generation(impl_->capacity_generation);
  }
  impl_->recompute_governed();
  impl_->recompute_saturation();
}

void Governor::set_resource_enabled(const ResourceId& res, bool enabled) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->resources.find(res);
  if (it == impl_->resources.end()) return;
  it->second.enabled = enabled;
  impl_->recompute_governed();
  impl_->recompute_saturation();
}

void Governor::invalidate_resource_capacity(const ResourceId& res) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->resources.find(res);
  if (it == impl_->resources.end()) return;
  it->second.spec.generation = next_generation(it->second.spec.generation);
  impl_->capacity_generation = next_generation(impl_->capacity_generation);
  impl_->recompute_governed();
  impl_->recompute_saturation();
}

std::vector<ResourceSnapshot> Governor::list_resources() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::vector<ResourceSnapshot> out;
  out.reserve(impl_->resources.size());
  for (const auto& [rid, r] : impl_->resources) {
    (void)rid;
    ResourceSnapshot s;
    s.id = r.spec.id;
    s.class_ = r.spec.class_;
    s.source = r.spec.source;
    s.destination = r.spec.destination;
    s.direction = r.spec.direction;
    s.nominal = r.spec.nominal;
    s.measured = Capacity::make(std::max(0.0, r.measured));
    s.governed = Capacity::make(std::max(0.0, r.governed));
    s.reserved = Capacity::make(std::max(0.0, r.reserved));
    s.allocated = Capacity::make(std::max(0.0, r.allocated));
    s.instantaneous_util = (r.spec.nominal.value() > 0.0)
                               ? (r.allocated / r.spec.nominal.value()) : 0.0;
    s.moving_average_util = std::clamp(r.moving_avg, 0.0, 1.0);
    s.queue_depth = r.qdepth;
    s.saturated = r.saturated;
    s.measured_latency_ms = r.latency_ms;
    s.confidence = r.confidence;
    s.provenance = r.provenance;
    double staleness = (r.last_ms == 0.0) ? 0.0 : (impl_->now() - r.last_ms);
    s.staleness_ms = staleness;
    s.staleness_threshold_ms = impl_->config.policy.fairness_window_ms;
    s.capacity_generation = r.spec.generation;
    s.health = r.health;
    s.enabled = r.enabled;
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const ResourceSnapshot& a, const ResourceSnapshot& b) {
    return a.id < b.id;
  });
  return out;
}

std::optional<ResourceSnapshot> Governor::resource(const ResourceId& res) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->resources.find(res);
  if (it == impl_->resources.end()) return std::nullopt;
  ResourceSnapshot s;
  s.id = it->second.spec.id;
  s.class_ = it->second.spec.class_;
  s.source = it->second.spec.source;
  s.destination = it->second.spec.destination;
  s.direction = it->second.spec.direction;
  s.nominal = it->second.spec.nominal;
  s.measured = Capacity::make(std::max(0.0, it->second.measured));
  s.governed = Capacity::make(std::max(0.0, it->second.governed));
  s.reserved = Capacity::make(std::max(0.0, it->second.reserved));
  s.allocated = Capacity::make(std::max(0.0, it->second.allocated));
  s.instantaneous_util = (it->second.spec.nominal.value() > 0.0)
                             ? (it->second.allocated / it->second.spec.nominal.value()) : 0.0;
  s.moving_average_util = std::clamp(it->second.moving_avg, 0.0, 1.0);
  s.queue_depth = it->second.qdepth;
  s.saturated = it->second.saturated;
  s.measured_latency_ms = it->second.latency_ms;
  s.confidence = it->second.confidence;
  s.provenance = it->second.provenance;
  double staleness = (it->second.last_ms == 0.0) ? 0.0 : (impl_->now() - it->second.last_ms);
  s.staleness_ms = staleness;
  s.staleness_threshold_ms = impl_->config.policy.fairness_window_ms;
  s.capacity_generation = it->second.spec.generation;
  s.health = it->second.health;
  s.enabled = it->second.enabled;
  return s;
}

// ---- paths ----------------------------------------------------------------
PathId Governor::add_path(const Path& path) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto err = validate_path(path);
  if (err) throw value_error("invalid path: " + *err);
  impl_->paths[path.id] = path;
  return path.id;
}

std::vector<Path> Governor::list_paths() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::vector<Path> out;
  out.reserve(impl_->paths.size());
  for (const auto& [pid, p] : impl_->paths) {
    (void)pid;
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const Path& a, const Path& b) { return a.id < b.id; });
  return out;
}

void Governor::invalidate_path(const PathId& path) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->paths.find(path);
  if (it == impl_->paths.end()) return;
  it->second.path_generation = next_generation(it->second.path_generation);
}

std::optional<PathAnalysis> Governor::analyze_path(const PathId& path) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto pit = impl_->paths.find(path);
  if (pit == impl_->paths.end()) return std::nullopt;
  const Path& p = pit->second;
  PathAnalysis a;
  a.path = p.id;
  a.capacity_generation = impl_->capacity_generation;
  double bottleneck = kMaxRate;
  std::optional<ResourceId> bottleneck_id;
  for (const PathHop& hop : p.hops) {
    auto rit = impl_->resources.find(hop.resource);
    if (rit == impl_->resources.end()) continue;
    a.total_requested += Capacity::make(0.0);
    if (rit->second.governed < bottleneck) {
      bottleneck = rit->second.governed;
      bottleneck_id = hop.resource;
    }
    // competing flows on this resource
    for (const auto& [fid, f] : impl_->flows) {
      if (f.state != FlowState::Running && f.state != FlowState::Throttled &&
          f.state != FlowState::Reserved)
        continue;
      auto fpit = impl_->paths.find(f.spec.path);
      if (fpit == impl_->paths.end()) continue;
      for (const PathHop& fh : fpit->second.hops) {
        if (fh.resource == hop.resource) {
          a.competing_flows.push_back(fid);
          a.total_requested += Capacity::make(f.spec.requested_preferred.value());
          break;
        }
      }
    }
  }
  a.bottleneck = bottleneck_id;
  a.limiting_resource = bottleneck_id;
  a.feasible = Capacity::make(std::max(0.0, bottleneck));
  a.saturated = (a.total_requested.value() > a.feasible.value() + kEps);
  // deduplicate competing flows
  std::sort(a.competing_flows.begin(), a.competing_flows.end());
  a.competing_flows.erase(std::unique(a.competing_flows.begin(), a.competing_flows.end()),
                          a.competing_flows.end());
  return a;
}

// ---- flows ----------------------------------------------------------------
AdmissionResult Governor::submit_flow(const FlowSpec& spec) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  AdmissionResult out;
  out.flow = spec.id;
  auto err = validate_flow_spec(spec);
  if (err) {
    out.decision = make_decision(spec, DecisionKind::Reject, 0.0, 0.0, false,
                                 "invalid flow spec: " + *err, std::nullopt);
    out.decision.kind_reason = *err;
    out.decision.reject_reason = *err;
    return out;
  }
  if (impl_->flows.count(spec.id)) {
    out.decision = make_decision(spec, DecisionKind::Reject, 0.0, 0.0, false,
                                 "duplicate flow id", std::nullopt);
    out.decision.reject_reason = "duplicate flow id";
    return out;
  }
  auto pit = impl_->paths.find(spec.path);
  if (pit == impl_->paths.end()) {
    out.decision = make_decision(spec, DecisionKind::Reject, 0.0, 0.0, false,
                                 "unknown path", std::nullopt);
    out.decision.reject_reason = "unknown path";
    return out;
  }

  impl_->recompute_governed();
  impl_->recompute_reserved();
  impl_->recompute_allocated();

  bool feasible = true;
  double bottleneck_avail = kMaxRate;
  std::optional<ResourceId> bottleneck;
  for (const PathHop& hop : pit->second.hops) {
    auto rit = impl_->resources.find(hop.resource);
    if (rit == impl_->resources.end()) {
      feasible = false;
      break;
    }
    double avail = rit->second.governed - rit->second.reserved - rit->second.allocated;
    if (avail < 0.0) avail = 0.0;
    if (avail + kEps < spec.requested_min.value()) {
      feasible = false;
    }
    if (avail < bottleneck_avail) {
      bottleneck_avail = avail;
      bottleneck = hop.resource;
    }
  }

  auto& tenant = impl_->tenants[spec.tenant];
  auto& workload = impl_->workloads[spec.workload];
  tenant.submitted++;
  workload.submitted++;

  if (!feasible) {
    // Defer or reject the flow. We park it in the queue so it can be admitted
    // later when capacity frees.
    bool reject = impl_->config.policy.reject_if_no_slack;
    FlowRT fr;
    fr.spec = spec;
    fr.admitted_ms = impl_->now();
    fr.last_ms = impl_->now();
    fr.state = FlowState::Queued;
    fr.burst_remaining = spec.burst_bytes;
    fr.starvation_ms = 0.0;
    impl_->flows[spec.id] = std::move(fr);
    out.decision = make_decision(
        spec, reject ? DecisionKind::Reject : DecisionKind::Defer, 0.0, bottleneck_avail,
        impl_->config.policy.defer_if_min_infeasible,
        reject ? "minimum guarantee impossible and reject_if_no_slack" : "minimum guarantee infeasible on path",
        bottleneck);
    out.decision.reservation_pressure = 0.0;
    if (reject) out.decision.reject_reason = "minimum guarantee infeasible on path";
    else out.decision.defer_reason = "minimum guarantee infeasible on path";
    return out;
  }

  // Feasible: admit. Grant = clamp(preferred, min, available).
  double grant = std::min({spec.requested_preferred.value(), spec.requested_max.value(),
                           bottleneck_avail});
  if (grant < spec.requested_min.value()) grant = spec.requested_min.value();
  grant = std::clamp(grant, spec.requested_min.value(), spec.requested_max.value());

  FlowRT fr;
  fr.spec = spec;
  fr.admitted_ms = impl_->now();
  fr.last_ms = impl_->now();
  fr.state = FlowState::Reserved;
  fr.granted = grant;
  fr.burst_remaining = spec.burst_bytes;
  fr.starvation_ms = 0.0;
  fr.last_grant_update_ms = impl_->now();

  Reservation rsv;
  rsv.id = impl_->gen.next<ReservationId>();
  rsv.flow = spec.id;
  rsv.attempt = spec.attempt;
  rsv.flow_generation = spec.generation;
  rsv.path = spec.path;
  rsv.epoch = impl_->epoch;
  rsv.capacity_generation = impl_->capacity_generation;
  rsv.state = ReservationState::Active;
  for (const PathHop& hop : pit->second.hops) {
    ResourceAllocation al;
    al.resource = hop.resource;
    al.allocated = Capacity::make(grant);
    rsv.allocations.push_back(al);
  }
  impl_->reservations[rsv.id] = rsv;
  fr.reservation = rsv.id;
  impl_->flows[spec.id] = std::move(fr);

  out.admitted = true;
  out.reservation = rsv;
  out.decision = make_decision(spec, DecisionKind::Admit, grant, bottleneck_avail,
                               (bottleneck_avail < spec.requested_preferred.value()),
                               "admitted with grant", bottleneck);
  out.decision.granted = Capacity::make(grant);
  out.decision.competing_flows.push_back(spec.id);

  impl_->recompute_reserved();
  impl_->recompute_allocated();
  impl_->recompute_saturation();
  return out;
}

bool Governor::cancel_flow(const FlowId& flow, std::string reason) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(flow);
  if (it == impl_->flows.end()) return false;
  FlowRT& f = it->second;
  if (f.state == FlowState::Completed || f.state == FlowState::Cancelled ||
      f.state == FlowState::Failed)
    return false;
  impl_->set_flow_state(f, FlowState::Cancelled);
  f.last_error = std::move(reason);
  // release any reservation exactly once
  if (f.reservation) impl_->release_reservation_internal(*f.reservation);
  impl_->recompute_reserved();
  impl_->recompute_allocated();
  impl_->recompute_saturation();
  return true;
}

bool Governor::resume_flow(const FlowId& flow) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(flow);
  if (it == impl_->flows.end()) return false;
  FlowRT& f = it->second;
  if (f.state != FlowState::Preempted && f.state != FlowState::Throttled)
    return false;
  // promote back to reserved/queued so tick re-admits it.
  if (!f.reservation) {
    // re-reserve for the path
    auto pit = impl_->paths.find(f.spec.path);
    if (pit != impl_->paths.end()) {
      Reservation rsv;
      rsv.id = impl_->gen.next<ReservationId>();
      rsv.flow = f.spec.id;
      rsv.attempt = f.spec.attempt;
      rsv.flow_generation = f.spec.generation;
      rsv.path = f.spec.path;
      rsv.epoch = impl_->epoch;
      rsv.capacity_generation = impl_->capacity_generation;
      rsv.state = ReservationState::Active;
      for (const PathHop& hop : pit->second.hops) {
        ResourceAllocation al;
        al.resource = hop.resource;
        al.allocated = Capacity::make(std::max(f.granted, f.spec.requested_min.value()));
        rsv.allocations.push_back(al);
      }
      impl_->reservations[rsv.id] = rsv;
      f.reservation = rsv.id;
    }
  }
  impl_->set_flow_state(f, FlowState::Reserved);
  return true;
}

// ---- scheduler ---------------------------------------------------------------
namespace {
struct ResContention {
  ResourceId res;
  std::vector<const FlowRT*> flows;
  std::vector<AllocEntry> entries;
};
}  // namespace

void Governor::tick() {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  Impl& g = *impl_;
  double t = g.now();
  g.advance_tenant_starvation(t);

  // Admit any deferred (queued) flows that are now feasible.
  {
    std::vector<FlowId> queued;
    for (const auto& [fid, f] : g.flows) {
      if (f.state == FlowState::Queued) queued.push_back(fid);
    }
    for (const FlowId& fid : queued) {
      auto it = g.flows.find(fid);
      if (it == g.flows.end() || it->second.state != FlowState::Queued) continue;
      g.admit_queued(it->second);
    }
  }

  g.recompute_governed();
  g.recompute_reserved();

  std::unordered_map<ResourceId, ResContention> contention;
  for (auto& [fid, f] : g.flows) {
    (void)fid;
    bool alloc = (f.state == FlowState::Running || f.state == FlowState::Throttled ||
                  f.state == FlowState::Reserved);
    if (!alloc) continue;
    auto pit = g.paths.find(f.spec.path);
    if (pit == g.paths.end()) continue;
    bool adds_forbidden = false;
    for (const PathHop& hop : pit->second.hops) {
      auto& rc = contention[hop.resource];
      rc.res = hop.resource;
      adds_forbidden = true;
    }
    if (!adds_forbidden) continue;
    for (const PathHop& hop : pit->second.hops) {
      auto& rc = contention[hop.resource];
      bool present = false;
      for (const FlowRT* p : rc.flows) {
        if (p->spec.id == f.spec.id) { present = true; break; }
      }
      if (present) continue;
      rc.flows.push_back(&f);
      AllocEntry e;
      e.flow = f.spec.id;
      e.weight = g.flow_weight(f, t);
      e.min = f.spec.requested_min.value();
      e.want = f.spec.requested_preferred.value();
      e.max = f.spec.requested_max.value();
      rc.entries.push_back(e);
    }
  }

  std::unordered_map<FlowId, double> grant_min;
  for (auto& [res, rc] : contention) {
    (void)res;
    auto rit = g.resources.find(rc.res);
    if (rit == g.resources.end()) continue;
    double cap = std::max(0.0, rit->second.governed - rit->second.reserved);
    AllocResult ar = allocate_weighted(rc.entries, cap);
    for (std::size_t i = 0; i < rc.entries.size(); ++i) {
      const FlowId& fid = rc.entries[i].flow;
      double gv = ar.granted[i];
      auto itm = grant_min.find(fid);
      if (itm == grant_min.end()) grant_min[fid] = gv;
      else if (gv < itm->second) itm->second = gv;
    }
  }

  for (auto& [fid, f] : g.flows) {
    (void)fid;
    bool alloc = (f.state == FlowState::Running || f.state == FlowState::Throttled ||
                  f.state == FlowState::Reserved);
    if (!alloc) continue;
    auto itm = grant_min.find(fid);
    double ng = (itm == grant_min.end()) ? 0.0 : itm->second;
    f.granted = ng;

    if (f.state == FlowState::Reserved) {
      // Dispatch to a worker if one backs the path; otherwise run locally.
      auto wit = g.pick_worker_for_path(f.spec.path);
      if (wit != g.workers.end()) {
        f.assigned_worker = wit->first;
        f.assigned_boot = wit->second.boot;
      }
      g.set_flow_state(f, FlowState::Running);
      f.last_grant_update_ms = t;
    } else if (f.state == FlowState::Running || f.state == FlowState::Throttled) {
      if (ng <= 0.0 && f.spec.preemptible) {
        g.set_flow_state(f, FlowState::Preempted);
      } else if (ng + kEps < f.spec.requested_min.value()) {
        g.set_flow_state(f, FlowState::Throttled);
      } else {
        g.set_flow_state(f, FlowState::Running);
      }
      f.last_grant_update_ms = t;
    }
  }

  g.recompute_reserved();
  g.recompute_allocated();
  g.recompute_saturation();
}

// ---- flow queries -------------------------------------------------------------
std::vector<FlowSnapshot> Governor::list_flows() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::vector<FlowSnapshot> out;
  out.reserve(impl_->flows.size());
  for (const auto& [fid, f] : impl_->flows) {
    (void)fid;
    FlowSnapshot s;
    s.spec = f.spec;
    s.state = f.state;
    s.granted = Capacity::make(std::max(0.0, f.granted));
    s.admitted_ms = f.admitted_ms;
    s.last_update_ms = f.last_ms;
    s.bytes_transferred = f.bytes_done;
    s.burst_remaining = f.burst_remaining;
    s.retry_count = f.retries;
    s.starvation_ms = f.starvation_ms;
    s.reservation = f.reservation;
    s.assigned_worker = f.assigned_worker;
    s.assigned_boot = f.assigned_boot;
    s.last_error = f.last_error;
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const FlowSnapshot& a, const FlowSnapshot& b) {
    return a.spec.id < b.spec.id;
  });
  return out;
}

std::optional<FlowSnapshot> Governor::flow(const FlowId& fl) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(fl);
  if (it == impl_->flows.end()) return std::nullopt;
  const FlowRT& f = it->second;
  FlowSnapshot s;
  s.spec = f.spec;
  s.state = f.state;
  s.granted = Capacity::make(std::max(0.0, f.granted));
  s.admitted_ms = f.admitted_ms;
  s.last_update_ms = f.last_ms;
  s.bytes_transferred = f.bytes_done;
  s.burst_remaining = f.burst_remaining;
  s.retry_count = f.retries;
  s.starvation_ms = f.starvation_ms;
  s.reservation = f.reservation;
  s.assigned_worker = f.assigned_worker;
  s.assigned_boot = f.assigned_boot;
  s.last_error = f.last_error;
  return s;
}

// ---- explainability ------------------------------------------------------------
Decision Governor::explain_flow(const FlowId& fl) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(fl);
  Decision d;
  d.flow = fl;
  d.kind = DecisionKind::Defer;
  if (it == impl_->flows.end()) {
    d.kind_reason = "unknown flow";
    return d;
  }
  const FlowRT& f = it->second;
  d.tenant = f.spec.tenant;
  d.workload = f.spec.workload;
  d.requested = f.spec.requested_preferred;
  d.granted = Capacity::make(std::max(0.0, f.granted));
  d.minimum_guarantee = f.spec.requested_min;
  d.preferred = f.spec.requested_preferred;
  d.maximum = f.spec.requested_max;
  d.path = f.spec.path;
  d.priority = f.spec.priority;
  d.latency_class = f.spec.latency_class;
  d.starvation_age = f.starvation_ms / 1000.0;
  d.burst_remaining = f.burst_remaining;
  switch (f.state) {
    case FlowState::Running: d.kind = DecisionKind::Grant; break;
    case FlowState::Throttled: d.kind = DecisionKind::Throttle; d.throttle_reason = "grant below minimum guarantee"; break;
    case FlowState::Preempted: d.kind = DecisionKind::Preempt; d.preemption_reason = "preempted to honor higher priority or guarantee"; break;
    case FlowState::Queued: d.kind = DecisionKind::Defer; d.defer_reason = "waiting for capacity"; break;
    case FlowState::Completed:
    case FlowState::Cancelled:
    case FlowState::Failed:
    default: d.kind = DecisionKind::Defer; break;
  }
  d.saturated = f.spec.requested_preferred.value() > f.granted + kEps;
  // bottleneck and competing flows
  auto pit = impl_->paths.find(f.spec.path);
  if (pit != impl_->paths.end()) {
    double bt = kMaxRate;
    for (const PathHop& hop : pit->second.hops) {
      auto rit = impl_->resources.find(hop.resource);
      if (rit == impl_->resources.end()) continue;
      if (rit->second.governed < bt) {
        bt = rit->second.governed;
        d.bottleneck = hop.resource;
      }
    }
    d.capacity_generation = impl_->capacity_generation;
    if (d.bottleneck) {
      auto rit = impl_->resources.find(*d.bottleneck);
      if (rit != impl_->resources.end()) d.resource_generation = rit->second.spec.generation;
    } else {
      d.resource_generation = impl_->capacity_generation;
    }
  }
  double w = impl_->flow_weight(f, impl_->now());
  d.factors.push_back({"composite_weight", w, 1.0, "arbitration weight from priority, latency, deadline, starvation, fairness"});
  d.factors.push_back({"granted", f.granted, 1.0, "current governed byte rate"});
  d.factors.push_back({"starvation_age", d.starvation_age, 1.0, "seconds waiting for service"});
  return d;
}

bool Governor::explain_admission(const FlowSpec& spec, Decision& out) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto err = validate_flow_spec(spec);
  if (err) {
    out = make_decision(spec, DecisionKind::Reject, 0.0, 0.0, false, *err, std::nullopt);
    out.reject_reason = *err;
    return true;
  }
  auto pit = impl_->paths.find(spec.path);
  if (pit == impl_->paths.end()) {
    out = make_decision(spec, DecisionKind::Reject, 0.0, 0.0, false, "unknown path", std::nullopt);
    out.reject_reason = "unknown path";
    return true;
  }
  double bottleneck_avail = kMaxRate;
  std::optional<ResourceId> bottleneck;
  bool feasible = true;
  double governed_cap = 0.0;
  for (const PathHop& hop : pit->second.hops) {
    auto rit = impl_->resources.find(hop.resource);
    if (rit == impl_->resources.end()) continue;
    double avail = std::max(0.0, rit->second.governed - rit->second.reserved -
                                 rit->second.allocated);
    governed_cap = rit->second.governed;
    if (avail + kEps < spec.requested_min.value()) feasible = false;
    if (avail < bottleneck_avail) {
      bottleneck_avail = avail;
      bottleneck = hop.resource;
    }
  }
  if (!feasible) {
    out = make_decision(spec, DecisionKind::Defer, 0.0, bottleneck_avail, true,
                        "minimum guarantee infeasible", bottleneck);
    out.defer_reason = "minimum guarantee infeasible";
    return true;
  }
  double grant = std::clamp(std::min({spec.requested_preferred.value(),
                                      spec.requested_max.value(), bottleneck_avail}),
                            spec.requested_min.value(), spec.requested_max.value());
  out = make_decision(spec, DecisionKind::Admit, grant, bottleneck_avail,
                      bottleneck_avail < spec.requested_preferred.value(),
                      "admitted", bottleneck);
  out.granted = Capacity::make(grant);
  return true;
}

// ---- reservations --------------------------------------------------------------
std::vector<Reservation> Governor::list_reservations() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::vector<Reservation> out;
  out.reserve(impl_->reservations.size());
  for (const auto& [rid, r] : impl_->reservations) {
    (void)rid;
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const Reservation& a, const Reservation& b) {
    return a.id < b.id;
  });
  return out;
}

std::optional<Reservation> Governor::reservation(const ReservationId& id) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->reservations.find(id);
  if (it == impl_->reservations.end()) return std::nullopt;
  return it->second;
}

bool Governor::release_reservation(const ReservationId& id) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->reservations.find(id);
  if (it == impl_->reservations.end()) return false;
  if (it->second.state == ReservationState::Released ||
      it->second.state == ReservationState::Cancelled ||
      it->second.state == ReservationState::Expired ||
      it->second.state == ReservationState::Failed)
    return false;  // exactly-once: already released
  impl_->release_reservation_internal(id);
  impl_->recompute_reserved();
  impl_->recompute_saturation();
  return true;
}

// ---- distributed authority -----------------------------------------------------
WorkerId Governor::register_worker(const WorkerRegistration& reg) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  Impl& g = *impl_;
  auto existing = g.workers.find(reg.worker);
  bool is_restart = false;
  if (existing != g.workers.end()) {
    is_restart = existing->second.alive && existing->second.boot != reg.boot;
  }
  WorkerRT& w = g.workers[reg.worker];
  w.id = reg.worker;
  w.boot = reg.boot;
  w.backend = reg.backend;
  w.protocol = reg.protocol_version;
  w.alive = true;
  w.back_resources.clear();
  for (const ResourceSpec& rs : reg.inventory) {
    ResRT& r = g.resources[rs.id];
    r.spec = rs;
    if (rs.generation.is_null())
      r.spec.generation = fresh_generation<CapacityGeneration>(g.gen.next64());
    r.measured = 0.0;
    r.last_ms = g.now();
    w.back_resources.push_back(rs.id);
  }
  if (is_restart) {
    // A worker restarted with a fresh boot: roll authority for any flow that was
    // running on this worker under the OLD boot. The old reservation is released
    // exactly once, a new AttemptId and a new generation-fenced reservation bound
    // to the new boot are created, and the flow is re-queued for dispatch.
    for (auto& [fid, f] : g.flows) {
      (void)fid;
      if (f.state != FlowState::Running || !f.assigned_worker ||
          *f.assigned_worker != reg.worker)
        continue;
      if (f.reservation) g.release_reservation_internal(*f.reservation);
      f.retries++;
      f.spec.attempt = g.gen.next<AttemptId>();
      f.spec.generation = next_generation(f.spec.generation);
      f.assigned_worker = reg.worker;
      f.assigned_boot = reg.boot;
      f.bytes_done = 0;
      f.last_error = "worker restarted; retry with new attempt";
      auto pit = g.paths.find(f.spec.path);
      if (pit != g.paths.end()) {
        Reservation rsv;
        rsv.id = g.gen.next<ReservationId>();
        rsv.flow = f.spec.id;
        rsv.attempt = f.spec.attempt;
        rsv.flow_generation = f.spec.generation;
        rsv.path = f.spec.path;
        rsv.epoch = g.epoch;
        rsv.worker_boot = reg.boot;
        rsv.capacity_generation = g.capacity_generation;
        rsv.state = ReservationState::Active;
        double grant = std::max(f.granted, f.spec.requested_min.value());
        if (grant <= 0.0) grant = f.spec.requested_preferred.value();
        for (const PathHop& hop : pit->second.hops) {
          ResourceAllocation al;
          al.resource = hop.resource;
          al.allocated = Capacity::make(grant);
          rsv.allocations.push_back(al);
        }
        g.reservations[rsv.id] = rsv;
        f.reservation = rsv.id;
        f.granted = grant;
      }
      g.set_flow_state(f, FlowState::Reserved);
    }
  }
  g.recompute_governed();
  return reg.worker;
}

void Governor::deregister_worker(const WorkerId& wn) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->workers.find(wn);
  if (it != impl_->workers.end()) it->second.alive = false;
  // Invalidate any running flows assigned to the lost worker so they are
  // re-dispatched (with a fresh attempt on the restarted boot) by the next tick.
  for (auto& [fid, f] : impl_->flows) {
    (void)fid;
    if (f.state == FlowState::Running && f.assigned_worker && *f.assigned_worker == wn) {
      f.assigned_worker.reset();
      f.last_error = "worker lost";
    }
  }
}

bool Governor::report_progress(const FlowId& flow, const AttemptId& attempt,
                               const FlowGeneration& fgen, const WorkerBootId& boot,
                               uint64_t bytes_transferred) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(flow);
  if (it == impl_->flows.end()) return false;
  FlowRT& f = it->second;
  // Authority validation: stale reports mutate nothing.
  if (f.state != FlowState::Running) return false;
  if (f.spec.attempt != attempt) return false;
  if (f.spec.generation != fgen) return false;
  if (f.assigned_worker) {
    auto wit = impl_->workers.find(*f.assigned_worker);
    if (wit == impl_->workers.end() || !wit->second.alive) return false;
    if (wit->second.boot != boot) return false;
  }
  // Fence against a stale coordinator epoch / capacity generation: a report
  // carries only attempt+fgen+boot, so additionally confirm the flow's
  // reservation was minted under the CURRENT authority. A stale epoch or a stale
  // capacity generation rejects the report and mutates nothing.
  if (f.reservation) {
    auto rit = impl_->reservations.find(*f.reservation);
    if (rit == impl_->reservations.end()) return false;
    if (rit->second.epoch != impl_->epoch ||
        rit->second.capacity_generation != impl_->capacity_generation)
      return false;
  }
  f.bytes_done = std::max(f.bytes_done, bytes_transferred);
  f.last_ms = impl_->now();
  return true;
}

bool Governor::report_completion(const FlowId& flow, const AttemptId& attempt,
                                 const FlowGeneration& fgen, const WorkerBootId& boot,
                                 uint64_t bytes_transferred) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->flows.find(flow);
  if (it == impl_->flows.end()) return false;
  FlowRT& f = it->second;
  if (f.state != FlowState::Running) return false;
  if (f.spec.attempt != attempt) return false;
  if (f.spec.generation != fgen) return false;
  if (f.assigned_worker) {
    auto wit = impl_->workers.find(*f.assigned_worker);
    if (wit == impl_->workers.end() || !wit->second.alive) return false;
    if (wit->second.boot != boot) return false;
  }
  // Fence against a stale coordinator epoch / capacity generation: a report
  // carries only attempt+fgen+boot, so additionally confirm the flow's
  // reservation was minted under the CURRENT authority. A stale epoch or a stale
  // capacity generation rejects the report and mutates nothing.
  if (f.reservation) {
    auto rit = impl_->reservations.find(*f.reservation);
    if (rit == impl_->reservations.end()) return false;
    if (rit->second.epoch != impl_->epoch ||
        rit->second.capacity_generation != impl_->capacity_generation)
      return false;
  }
  f.bytes_done = std::max(f.bytes_done, bytes_transferred);
  impl_->set_flow_state(f, FlowState::Completed);
  f.last_ms = impl_->now();
  // settle accounting
  auto& t = impl_->tenants[f.spec.tenant];
  auto& w = impl_->workloads[f.spec.workload];
  t.completed++;
  w.completed++;
  t.served_bytes += static_cast<double>(f.spec.byte_count);
  w.served_bytes += static_cast<double>(f.spec.byte_count);
  // release reservation exactly once
  if (f.reservation) impl_->release_reservation_internal(*f.reservation);
  impl_->recompute_reserved();
  impl_->recompute_allocated();
  impl_->recompute_saturation();
  return true;
}

// ---- accounting diagnostics -----------------------------------------------------
std::map<TenantId, double> Governor::tenant_allocations() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::map<TenantId, double> out;
  for (const auto& [tid, t] : impl_->tenants) out[tid] = t.allocated_now;
  // recompute on the fly for correctness
  for (const auto& [fid, f] : impl_->flows) {
    (void)fid;
    if (f.state != FlowState::Running && f.state != FlowState::Throttled) continue;
    out[f.spec.tenant] += f.granted;
  }
  return out;
}

std::map<WorkloadId, double> Governor::workload_allocations() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::map<WorkloadId, double> out;
  for (const auto& [wid, w] : impl_->workloads) out[wid] = w.allocated_now;
  for (const auto& [fid, f] : impl_->flows) {
    (void)fid;
    if (f.state != FlowState::Running && f.state != FlowState::Throttled) continue;
    out[f.spec.workload] += f.granted;
  }
  return out;
}

bool Governor::accounting_at_zero() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  for (const auto& [rid, r] : impl_->resources) {
    (void)rid;
    if (r.allocated > kEps || r.reserved > kEps) return false;
  }
  for (const auto& [fid, f] : impl_->flows) {
    (void)fid;
    if (f.state == FlowState::Running || f.state == FlowState::Throttled ||
        f.state == FlowState::Reserved)
      return false;
  }
  for (const auto& [rid, r] : impl_->reservations) {
    (void)rid;
    if (r.state == ReservationState::Active || r.state == ReservationState::Pending)
      return false;
  }
  return true;
}

// ---- snapshot builders ------------------------------------------------------
namespace {
ResourceSnapshot to_resource_snap(const ResRT& r, double now_ms, double threshold) {
  ResourceSnapshot s;
  s.id = r.spec.id;
  s.class_ = r.spec.class_;
  s.source = r.spec.source;
  s.destination = r.spec.destination;
  s.direction = r.spec.direction;
  s.nominal = r.spec.nominal;
  s.measured = Capacity::make(std::max(0.0, r.measured));
  s.governed = Capacity::make(std::max(0.0, r.governed));
  s.reserved = Capacity::make(std::max(0.0, r.reserved));
  s.allocated = Capacity::make(std::max(0.0, r.allocated));
  s.instantaneous_util = (r.spec.nominal.value() > 0.0) ? (r.allocated / r.spec.nominal.value())
                                                        : 0.0;
  s.moving_average_util = std::clamp(r.moving_avg, 0.0, 1.0);
  s.queue_depth = r.qdepth;
  s.saturated = r.saturated;
  s.measured_latency_ms = r.latency_ms;
  s.confidence = r.confidence;
  s.provenance = r.provenance;
  double staleness = (r.last_ms == 0.0) ? 0.0 : (now_ms - r.last_ms);
  s.staleness_ms = staleness;
  s.staleness_threshold_ms = threshold;
  s.capacity_generation = r.spec.generation;
  s.health = r.health;
  s.enabled = r.enabled;
  return s;
}

FlowSnapshot to_flow_snap(const FlowRT& f, double now_ms) {
  (void)now_ms;
  FlowSnapshot s;
  s.spec = f.spec;
  s.state = f.state;
  s.granted = Capacity::make(std::max(0.0, f.granted));
  s.admitted_ms = f.admitted_ms;
  s.last_update_ms = f.last_ms;
  s.bytes_transferred = f.bytes_done;
  s.burst_remaining = f.burst_remaining;
  s.retry_count = f.retries;
  s.starvation_ms = f.starvation_ms;
  s.reservation = f.reservation;
  s.assigned_worker = f.assigned_worker;
  s.assigned_boot = f.assigned_boot;
  s.last_error = f.last_error;
  return s;
}

}  // namespace

GovernorSnapshot Governor::build_snapshot_locked() const {
  const Impl& g = *impl_;
  GovernorSnapshot s;
  s.format_version = kFormatVersion;
  s.coordinator = g.coordinator;
  s.epoch = g.epoch;
  s.capacity_generation = g.capacity_generation;
  s.policy = g.config.policy;
  s.saved_at_ms = g.now();
  for (const auto& [id, r] : g.resources) {
    (void)id;
    s.resources.push_back(r.spec);
    s.resource_state[r.spec.id] = to_resource_snap(r, s.saved_at_ms,
                                                   g.config.policy.fairness_window_ms);
  }
  for (const auto& [id, p] : g.paths) { (void)id; s.paths.push_back(p); }
  for (const auto& [id, f] : g.flows) { (void)id; s.flows.push_back(to_flow_snap(f, s.saved_at_ms)); }
  for (const auto& [id, r] : g.reservations) { (void)id; s.reservations.push_back(r); }
  for (const auto& [tid, t] : g.tenants) {
    TenantAccounting ta;
    ta.tenant = tid;
    ta.allocated_now = t.allocated_now;
    ta.served_bytes = t.served_bytes;
    ta.submitted = t.submitted;
    ta.completed = t.completed;
    ta.failed = t.failed;
    ta.weight = t.weight;
    s.tenants[tid] = ta;
  }
  for (const auto& [wid, w] : g.workloads) {
    WorkloadAccounting wa;
    wa.workload = wid;
    wa.allocated_now = w.allocated_now;
    wa.served_bytes = w.served_bytes;
    wa.submitted = w.submitted;
    wa.completed = w.completed;
    wa.failed = w.failed;
    wa.weight = w.weight;
    s.workloads[wid] = wa;
  }
  return s;
}

GovernorSnapshot Governor::snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return build_snapshot_locked();
}

void Governor::restore(const GovernorSnapshot& snap) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  Impl& g = *impl_;
  g.coordinator = snap.coordinator;
  g.epoch = snap.epoch;
  g.capacity_generation = snap.capacity_generation;
  g.config.policy = snap.policy;
  g.resources.clear();
  for (const ResourceSpec& rs : snap.resources) {
    ResRT r;
    r.spec = rs;
    r.last_ms = g.now();
    g.resources[rs.id] = r;
  }
  g.paths.clear();
  for (const Path& p : snap.paths) g.paths[p.id] = p;
  g.flows.clear();
  for (const FlowSnapshot& fs : snap.flows) {
    FlowRT fr;
    fr.spec = fs.spec;
    fr.state = fs.state;
    fr.admitted_ms = fs.admitted_ms;
    fr.last_ms = fs.last_update_ms;
    fr.granted = fs.granted.value();
    fr.bytes_done = fs.bytes_transferred;
    fr.burst_remaining = fs.burst_remaining;
    fr.retries = fs.retry_count;
    fr.starvation_ms = fs.starvation_ms;
    fr.reservation = fs.reservation;
    g.flows[fs.spec.id] = fr;
  }
  g.reservations.clear();
  for (const Reservation& r : snap.reservations) g.reservations[r.id] = r;
  g.tenants.clear();
  for (const auto& [tid, ta] : snap.tenants) {
    TenantAcc t;
    t.allocated_now = ta.allocated_now;
    t.served_bytes = ta.served_bytes;
    t.submitted = ta.submitted;
    t.completed = ta.completed;
    t.failed = ta.failed;
    t.weight = ta.weight;
    g.tenants[tid] = t;
  }
  g.workloads.clear();
  for (const auto& [wid, wa] : snap.workloads) {
    WorkloadAcc w;
    w.allocated_now = wa.allocated_now;
    w.served_bytes = wa.served_bytes;
    w.submitted = wa.submitted;
    w.completed = wa.completed;
    w.failed = wa.failed;
    w.weight = wa.weight;
    g.workloads[wid] = w;
  }
  g.recompute_governed();
  g.recompute_reserved();
  g.recompute_allocated();
  g.recompute_saturation();
}

std::vector<uint8_t> Governor::save() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return encode_snapshot(build_snapshot_locked());
}

void Governor::load(const uint8_t* data, size_t n) {
  GovernorSnapshot snap = decode_snapshot(data, n);
  restore(snap);
}

void Governor::save_file(const std::string& path) const {
  std::vector<uint8_t> bytes = save();  // encode without holding the lock
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw error("could not open state file for write: " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) throw error("failed to write state file: " + path);
    f.flush();
    if (!f) throw error("failed to flush state file: " + path);
  }
  // atomic replace on the same volume.
  std::ifstream check(tmp, std::ios::binary);
  if (check) {
    check.seekg(0, std::ios::end);
    auto sz = check.tellg();
    if (sz != static_cast<std::streampos>(bytes.size()))
      throw error("state file size mismatch after write: " + path);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) throw error("failed to atomically replace state file: " + path);
}

void Governor::load_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw error("could not open state file for read: " + path);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  if (data.empty()) throw value_error("state file is empty");
  load(data.data(), data.size());
}

}  // namespace bg


