// Bandwidth Governor - framed wire protocol implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/wire.hpp"

#include <cstring>

namespace bg {

const char* wire_type_name(WireType t) noexcept {
  switch (t) {
    case WireType::Register: return "register";
    case WireType::RegisterAck: return "register_ack";
    case WireType::SubmitFlow: return "submit_flow";
    case WireType::SubmitAck: return "submit_ack";
    case WireType::FlowDispatch: return "flow_dispatch";
    case WireType::FlowGrant: return "flow_grant";
    case WireType::FlowCancel: return "flow_cancel";
    case WireType::Progress: return "progress";
    case WireType::Completion: return "completion";
    case WireType::Ack: return "ack";
    case WireType::QueryResources: return "query_resources";
    case WireType::QueryFlows: return "query_flows";
    case WireType::QueryReservations: return "query_reservations";
    case WireType::Snapshot: return "snapshot";
    case WireType::Save: return "save";
    case WireType::Load: return "load";
    case WireType::Response: return "response";
    case WireType::RegisterReject: return "register_reject";
    case WireType::Shutdown: return "shutdown";
  }
  return "unknown";
}

namespace {
uint32_t frame_crc(const std::vector<uint8_t>& hdr, const uint8_t* payload, size_t len) {
  Crc32 c;
  c.update(hdr.data(), hdr.size());
  c.update(payload, len);
  return c.value();
}
}  // namespace

std::vector<uint8_t> encode_frame(WireType type, const std::vector<uint8_t>& payload) {
  BinarySink s;
  s.u32(kWireMagic);
  s.u32(kWireVersion);
  s.u32(static_cast<uint32_t>(type));
  s.u32(static_cast<uint32_t>(payload.size()));
  std::vector<uint8_t> crc_hdr;
  {
    BinarySink h;
    h.u32(static_cast<uint32_t>(type));
    h.u32(static_cast<uint32_t>(payload.size()));
    crc_hdr = h.data();
  }
  s.bytes(payload.data(), payload.size());
  s.u32(frame_crc(crc_hdr, payload.data(), payload.size()));
  return s.data();
}

DecodedFrame decode_frame(const uint8_t* data, size_t n) {
  BinarySource s(data, n);
  uint32_t magic = s.u32();
  if (magic != kWireMagic) throw value_error("bad wire frame magic");
  uint32_t ver = s.u32();
  if (ver != kWireVersion) throw value_error("incompatible wire protocol version");
  uint32_t type = s.u32();
  uint32_t len = s.u32();
  if (len > s.remaining()) throw value_error("frame payload length exceeds buffer");
  std::string_view pv = s.raw(len);
  std::vector<uint8_t> payload(pv.begin(), pv.end());
  uint32_t crc = s.u32();
  if (s.remaining() != 0) throw value_error("trailing garbage in wire frame");
  std::vector<uint8_t> crc_hdr;
  {
    BinarySink h;
    h.u32(type);
    h.u32(len);
    crc_hdr = h.data();
  }
  if (crc != frame_crc(crc_hdr, payload.data(), payload.size()))
    throw value_error("wire frame checksum mismatch");
  DecodedFrame df;
  df.type = static_cast<WireType>(type);
  df.payload = std::move(payload);
  return df;
}

// ---------------------------------------------------------------------------
// payload serialisers
// ---------------------------------------------------------------------------
namespace payload {

namespace {
void w_res(BinarySink& s, const ResourceSpec& r) {
  s.id(r.id);
  s.u32(static_cast<uint32_t>(r.class_));
  s.str(r.source);
  s.str(r.destination);
  s.u32(static_cast<uint32_t>(r.direction));
  s.f64(r.nominal.value());
  s.id(r.generation);
  s.u8(r.enabled ? 1 : 0);
}
ResourceSpec r_res(BinarySource& s) {
  ResourceSpec r;
  r.id = s.id<ResourceId>();
  r.class_ = static_cast<ResourceClass>(s.u32());
  r.source = s.str();
  r.destination = s.str();
  r.direction = static_cast<Directionality>(s.u32());
  r.nominal = Capacity::make(s.f64());
  r.generation = s.id<CapacityGeneration>();
  r.enabled = s.u8() != 0;
  return r;
}
void w_spec(BinarySink& s, const FlowSpec& f) {
  s.id(f.id);
  s.id(f.tenant);
  s.id(f.workload);
  s.id(f.attempt);
  s.id(f.generation);
  s.str(f.source);
  s.str(f.destination);
  s.id(f.path);
  s.u64(f.byte_count);
  s.u32(static_cast<uint32_t>(f.direction));
  s.i64(f.priority);
  s.u32(static_cast<uint32_t>(f.latency_class));
  s.f64(f.deadline_seconds);
  s.capacity(f.requested_min);
  s.capacity(f.requested_preferred);
  s.capacity(f.requested_max);
  s.u64(f.burst_bytes);
  s.u8(f.preemptible ? 1 : 0);
  s.u8(f.resumable ? 1 : 0);
}
FlowSpec r_spec(BinarySource& s) {
  FlowSpec f;
  f.id = s.id<FlowId>();
  f.tenant = s.id<TenantId>();
  f.workload = s.id<WorkloadId>();
  f.attempt = s.id<AttemptId>();
  f.generation = s.id<FlowGeneration>();
  f.source = s.str();
  f.destination = s.str();
  f.path = s.id<PathId>();
  f.byte_count = s.u64();
  f.direction = static_cast<Directionality>(s.u32());
  f.priority = static_cast<int>(s.i64());
  f.latency_class = static_cast<LatencyClass>(s.u32());
  f.deadline_seconds = s.f64();
  f.requested_min = s.capacity();
  f.requested_preferred = s.capacity();
  f.requested_max = s.capacity();
  f.burst_bytes = s.u64();
  f.preemptible = s.u8() != 0;
  f.resumable = s.u8() != 0;
  return f;
}
void w_res_alloc(BinarySink& s, const ResourceAllocation& a) {
  s.id(a.resource);
  s.capacity(a.allocated);
}
ResourceAllocation r_res_alloc(BinarySource& s) {
  ResourceAllocation a;
  a.resource = s.id<ResourceId>();
  a.allocated = s.capacity();
  return a;
}
void w_reservation(BinarySink& s, const Reservation& r) {
  s.id(r.id);
  s.id(r.flow);
  s.id(r.attempt);
  s.id(r.flow_generation);
  s.id(r.path);
  s.u32(static_cast<uint32_t>(r.allocations.size()));
  for (const auto& a : r.allocations) w_res_alloc(s, a);
  s.id(r.epoch);
  s.id(r.worker_boot);
  s.id(r.capacity_generation);
  s.u32(static_cast<uint32_t>(r.state));
}
Reservation r_reservation(BinarySource& s) {
  Reservation r;
  r.id = s.id<ReservationId>();
  r.flow = s.id<FlowId>();
  r.attempt = s.id<AttemptId>();
  r.flow_generation = s.id<FlowGeneration>();
  r.path = s.id<PathId>();
  uint32_t n = s.u32();
  for (uint32_t i = 0; i < n; ++i) r.allocations.push_back(r_res_alloc(s));
  r.epoch = s.id<CoordinatorEpoch>();
  r.worker_boot = s.id<WorkerBootId>();
  r.capacity_generation = s.id<CapacityGeneration>();
  r.state = static_cast<ReservationState>(s.u32());
  return r;
}
void w_decision(BinarySink& s, const Decision& d) {
  s.id(d.flow);
  s.id(d.tenant);
  s.id(d.workload);
  s.u32(static_cast<uint32_t>(d.kind));
  s.capacity(d.requested);
  s.capacity(d.granted);
  s.capacity(d.minimum_guarantee);
  s.capacity(d.preferred);
  s.capacity(d.maximum);
  s.u8(d.bottleneck ? 1 : 0);
  if (d.bottleneck) s.id(*d.bottleneck);
  s.id(d.path);
  s.i64(d.priority);
  s.u32(static_cast<uint32_t>(d.latency_class));
  s.f64(d.deadline_pressure);
  s.f64(d.tenant_fairness);
  s.f64(d.workload_fairness);
  s.f64(d.starvation_age);
  s.u64(d.burst_remaining);
  s.u8(d.saturated ? 1 : 0);
  s.u32(static_cast<uint32_t>(d.competing_flows.size()));
  for (const FlowId& fd : d.competing_flows) s.id(fd);
  s.f64(d.reservation_pressure);
  s.id(d.resource_generation);
  s.id(d.capacity_generation);
  s.str(d.kind_reason);
  s.str(d.defer_reason);
  s.str(d.reject_reason);
  s.str(d.throttle_reason);
  s.str(d.preemption_reason);
  s.u32(static_cast<uint32_t>(d.factors.size()));
  for (const DecisionFactor& f : d.factors) {
    s.str(f.name);
    s.f64(f.value);
    s.f64(f.weight);
    s.str(f.rationale);
  }
}
Decision r_decision(BinarySource& s) {
  Decision d;
  d.flow = s.id<FlowId>();
  d.tenant = s.id<TenantId>();
  d.workload = s.id<WorkloadId>();
  d.kind = static_cast<DecisionKind>(s.u32());
  d.requested = s.capacity();
  d.granted = s.capacity();
  d.minimum_guarantee = s.capacity();
  d.preferred = s.capacity();
  d.maximum = s.capacity();
  bool has_b = s.u8() != 0;
  if (has_b) d.bottleneck = s.id<ResourceId>();
  d.path = s.id<PathId>();
  d.priority = static_cast<int>(s.i64());
  d.latency_class = static_cast<LatencyClass>(s.u32());
  d.deadline_pressure = s.f64();
  d.tenant_fairness = s.f64();
  d.workload_fairness = s.f64();
  d.starvation_age = s.f64();
  d.burst_remaining = s.u64();
  d.saturated = s.u8() != 0;
  uint32_t nc = s.u32();
  for (uint32_t i = 0; i < nc; ++i) d.competing_flows.push_back(s.id<FlowId>());
  d.reservation_pressure = s.f64();
  d.resource_generation = s.id<CapacityGeneration>();
  d.capacity_generation = s.id<CapacityGeneration>();
  d.kind_reason = s.str();
  d.defer_reason = s.str();
  d.reject_reason = s.str();
  d.throttle_reason = s.str();
  d.preemption_reason = s.str();
  uint32_t nf = s.u32();
  for (uint32_t i = 0; i < nf; ++i) {
    DecisionFactor f;
    f.name = s.str();
    f.value = s.f64();
    f.weight = s.f64();
    f.rationale = s.str();
    d.factors.push_back(std::move(f));
  }
  return d;
}
}  // namespace

std::vector<uint8_t> encode_register(const Register& r) {
  BinarySink s;
  s.id(r.worker);
  s.id(r.boot);
  s.u32(r.protocol);
  s.str(r.backend);
  s.u32(static_cast<uint32_t>(r.inventory.size()));
  for (const auto& rs : r.inventory) w_res(s, rs);
  return s.data();
}
Register decode_register(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Register r;
  r.worker = s.id<WorkerId>();
  r.boot = s.id<WorkerBootId>();
  r.protocol = s.u32();
  r.backend = s.str();
  uint32_t cnt = s.u32();
  for (uint32_t i = 0; i < cnt; ++i) r.inventory.push_back(r_res(s));
  return r;
}

std::vector<uint8_t> encode_register_ack(const RegisterAck& a) {
  BinarySink s;
  s.u8(a.ok ? 1 : 0);
  s.id(a.coordinator);
  s.id(a.epoch);
  s.id(a.generation);
  s.str(a.reason);
  return s.data();
}
RegisterAck decode_register_ack(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  RegisterAck a;
  a.ok = s.u8() != 0;
  a.coordinator = s.id<CoordinatorId>();
  a.epoch = s.id<CoordinatorEpoch>();
  a.generation = s.id<CapacityGeneration>();
  a.reason = s.str();
  return a;
}

std::vector<uint8_t> encode_flow_spec(const FlowSpec& f) {
  BinarySink s;
  w_spec(s, f);
  return s.data();
}
FlowSpec decode_flow_spec(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  return r_spec(s);
}

std::vector<uint8_t> encode_admission(const AdmissionWire& a) {
  BinarySink s;
  s.u8(a.admitted ? 1 : 0);
  w_spec(s, a.flow);
  w_decision(s, a.decision);
  s.u8(a.reservation ? 1 : 0);
  if (a.reservation) w_reservation(s, *a.reservation);
  return s.data();
}
AdmissionWire decode_admission(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  AdmissionWire a;
  a.admitted = s.u8() != 0;
  a.flow = r_spec(s);
  a.decision = r_decision(s);
  bool has = s.u8() != 0;
  if (has) a.reservation = r_reservation(s);
  return a;
}

std::vector<uint8_t> encode_dispatch(const Dispatch& d) {
  BinarySink s;
  w_spec(s, d.flow);
  s.id(d.attempt);
  s.id(d.fgen);
  s.id(d.boot);
  s.capacity(d.grant);
  s.id(d.reservation);
  return s.data();
}
Dispatch decode_dispatch(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Dispatch d;
  d.flow = r_spec(s);
  d.attempt = s.id<AttemptId>();
  d.fgen = s.id<FlowGeneration>();
  d.boot = s.id<WorkerBootId>();
  d.grant = s.capacity();
  d.reservation = s.id<ReservationId>();
  return d;
}

std::vector<uint8_t> encode_grant(const Grant& g) {
  BinarySink s;
  s.id(g.flow);
  s.id(g.attempt);
  s.id(g.fgen);
  s.id(g.boot);
  s.capacity(g.grant);
  return s.data();
}
Grant decode_grant(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Grant g;
  g.flow = s.id<FlowId>();
  g.attempt = s.id<AttemptId>();
  g.fgen = s.id<FlowGeneration>();
  g.boot = s.id<WorkerBootId>();
  g.grant = s.capacity();
  return g;
}

std::vector<uint8_t> encode_report(const Report& r) {
  BinarySink s;
  s.id(r.flow);
  s.id(r.attempt);
  s.id(r.fgen);
  s.id(r.boot);
  s.u64(r.bytes);
  s.u8(r.completed ? 1 : 0);
  return s.data();
}
Report decode_report(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Report r;
  r.flow = s.id<FlowId>();
  r.attempt = s.id<AttemptId>();
  r.fgen = s.id<FlowGeneration>();
  r.boot = s.id<WorkerBootId>();
  r.bytes = s.u64();
  r.completed = s.u8() != 0;
  return r;
}

std::vector<uint8_t> encode_ack(const Ack& a) {
  BinarySink s;
  s.u8(a.ok ? 1 : 0);
  s.str(a.reason);
  return s.data();
}
Ack decode_ack(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Ack a;
  a.ok = s.u8() != 0;
  a.reason = s.str();
  return a;
}

std::vector<uint8_t> encode_response(const Response& r) {
  BinarySink s;
  s.u8(r.ok ? 1 : 0);
  s.str(r.message);
  s.u32(static_cast<uint32_t>(r.body.size()));
  s.bytes(r.body.data(), r.body.size());
  return s.data();
}
Response decode_response(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  Response r;
  r.ok = s.u8() != 0;
  r.message = s.str();
  uint32_t len = s.u32();
  std::string_view sv = s.raw(len);
  r.body.assign(sv.begin(), sv.end());
  return r;
}

std::vector<uint8_t> encode_resource_snapshot(const std::vector<ResourceSnapshot>& v) {
  BinarySink s;
  s.u32(static_cast<uint32_t>(v.size()));
  for (const ResourceSnapshot& r : v) {
    s.id(r.id);
    s.u32(static_cast<uint32_t>(r.class_));
    s.str(r.source);
    s.str(r.destination);
    s.u32(static_cast<uint32_t>(r.direction));
    s.capacity(r.nominal);
    s.capacity(r.measured);
    s.capacity(r.governed);
    s.capacity(r.reserved);
    s.capacity(r.allocated);
    s.f64(r.instantaneous_util);
    s.f64(r.moving_average_util);
    s.u64(r.queue_depth);
    s.u8(r.saturated ? 1 : 0);
    s.f64(r.measured_latency_ms);
    s.f64(r.confidence);
    s.str(r.provenance);
    s.f64(r.staleness_ms);
    s.f64(r.staleness_threshold_ms);
    s.id(r.capacity_generation);
    s.u32(static_cast<uint32_t>(r.health));
    s.u8(r.enabled ? 1 : 0);
  }
  return s.data();
}
std::vector<ResourceSnapshot> decode_resource_snapshot(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  uint32_t cnt = s.u32();
  std::vector<ResourceSnapshot> v;
  v.reserve(cnt);
  for (uint32_t i = 0; i < cnt; ++i) {
    ResourceSnapshot r;
    r.id = s.id<ResourceId>();
    r.class_ = static_cast<ResourceClass>(s.u32());
    r.source = s.str();
    r.destination = s.str();
    r.direction = static_cast<Directionality>(s.u32());
    r.nominal = s.capacity();
    r.measured = s.capacity();
    r.governed = s.capacity();
    r.reserved = s.capacity();
    r.allocated = s.capacity();
    r.instantaneous_util = s.f64();
    r.moving_average_util = s.f64();
    r.queue_depth = s.u64();
    r.saturated = s.u8() != 0;
    r.measured_latency_ms = s.f64();
    r.confidence = s.f64();
    r.provenance = s.str();
    r.staleness_ms = s.f64();
    r.staleness_threshold_ms = s.f64();
    r.capacity_generation = s.id<CapacityGeneration>();
    r.health = static_cast<ResourceHealth>(s.u32());
    r.enabled = s.u8() != 0;
    v.push_back(std::move(r));
  }
  return v;
}

std::vector<uint8_t> encode_flow_snapshot(const std::vector<FlowSnapshot>& v) {
  BinarySink s;
  s.u32(static_cast<uint32_t>(v.size()));
  for (const FlowSnapshot& f : v) {
    w_spec(s, f.spec);
    s.u32(static_cast<uint32_t>(f.state));
    s.capacity(f.granted);
    s.f64(f.admitted_ms);
    s.f64(f.last_update_ms);
    s.u64(f.bytes_transferred);
    s.u64(f.burst_remaining);
    s.u64(f.retry_count);
    s.f64(f.starvation_ms);
    s.u8(f.reservation ? 1 : 0);
    if (f.reservation) s.id(*f.reservation);
    s.u8(f.assigned_worker ? 1 : 0);
    if (f.assigned_worker) s.id(*f.assigned_worker);
    s.id(f.assigned_boot);
    s.str(f.last_error);
  }
  return s.data();
}
std::vector<FlowSnapshot> decode_flow_snapshot(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  uint32_t cnt = s.u32();
  std::vector<FlowSnapshot> v;
  v.reserve(cnt);
  for (uint32_t i = 0; i < cnt; ++i) {
    FlowSnapshot f;
    f.spec = r_spec(s);
    f.state = static_cast<FlowState>(s.u32());
    f.granted = s.capacity();
    f.admitted_ms = s.f64();
    f.last_update_ms = s.f64();
    f.bytes_transferred = s.u64();
    f.burst_remaining = s.u64();
    f.retry_count = s.u64();
    f.starvation_ms = s.f64();
    bool has = s.u8() != 0;
    if (has) f.reservation = s.id<ReservationId>();
    bool has_w = s.u8() != 0;
    if (has_w) f.assigned_worker = s.id<WorkerId>();
    f.assigned_boot = s.id<WorkerBootId>();
    f.last_error = s.str();
    v.push_back(std::move(f));
  }
  return v;
}

std::vector<uint8_t> encode_reservation_snapshot(const std::vector<Reservation>& v) {
  BinarySink s;
  s.u32(static_cast<uint32_t>(v.size()));
  for (const Reservation& r : v) w_reservation(s, r);
  return s.data();
}
std::vector<Reservation> decode_reservation_snapshot(const uint8_t* p, size_t n) {
  BinarySource s(p, n);
  uint32_t cnt = s.u32();
  std::vector<Reservation> v;
  v.reserve(cnt);
  for (uint32_t i = 0; i < cnt; ++i) v.push_back(r_reservation(s));
  return v;
}

}  // namespace payload
}  // namespace bg
