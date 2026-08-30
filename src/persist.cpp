// Bandwidth Governor - persistence encoding implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/persist.hpp"
#include "bg/governor.hpp"

#include <cstring>
#include <set>
#include <unordered_set>

namespace bg {

// ---------------------------------------------------------------------------
// CRC32 (IEEE)
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kCrcTable[256] = {
  0x00000000u,0x77073096u,0xEE0E612Cu,0x990951BAu,0x076DC419u,0x706AF48Fu,
  0xE963A535u,0x9E6495A3u,0x0EDB8832u,0x79DCB8A4u,0xE0D5E91Eu,0x97D2D988u,
  0x09B64C2Bu,0x7EB17CBDu,0xE7B82D07u,0x90BF1D91u,0x1DB71064u,0x6AB020F2u,
  0xF3B97148u,0x84BE41DEu,0x1ADAD47Du,0x6DDDE4EBu,0xF4D4B551u,0x83D385C7u,
  0x136C9856u,0x646BA8C0u,0xFD62F97Au,0x8A65C9ECu,0x14015C4Fu,0x63066CD9u,
  0xFA0F3D63u,0x8D080DF5u,0x3B6E20C8u,0x4C69105Eu,0xD56041E4u,0xA2677172u,
  0x3C03E4D1u,0x4B04D447u,0xD20D85FDu,0xA50AB56Bu,0x35B5A8FAu,0x42B2986Cu,
  0xDBBBC9D6u,0xACBCF940u,0x32D86CE3u,0x45DF5C75u,0xDCD60DCFu,0xABD13D59u,
  0x26D930ACu,0x51DE003Au,0xC8D75180u,0xBFD06116u,0x21B4F4B5u,0x56B3C423u,
  0xCFBA9599u,0xB8BDA50Fu,0x2802B89Eu,0x5F058808u,0xC60CD9B2u,0xB10BE924u,
  0x2F6F7C87u,0x58684C11u,0xC1611DABu,0xB6662D3Du,0x76DC4190u,0x01DB7106u,
  0x98D220BCu,0xEFD5102Au,0x71B18589u,0x06B6B51Fu,0x9FBFE4A5u,0xE8B8D433u,
  0x7807C9A2u,0x0F00F934u,0x9609A88Eu,0xE10E9818u,0x7F6A0DBBu,0x086D3D2Du,
  0x91646C97u,0xE6635C01u,0x6B6B51F4u,0x1C6C6162u,0x856530D8u,0xF262004Eu,
  0x6C0695EDu,0x1B01A57Bu,0x8208F4C1u,0xF50FC457u,0x65B0D9C6u,0x12B7E950u,
  0x8BBEB8EAu,0xFCB9887Cu,0x62DD1DDFu,0x15DA2D49u,0x8CD37CF3u,0xFBD44C65u,
  0x4DB26158u,0x3AB551CEu,0xA3BC0074u,0xD4BB30E2u,0x4ADFA541u,0x3DD895D7u,
  0xA4D1C46Du,0xD3D6F4FBu,0x4369E96Au,0x346ED9FCu,0xAD678846u,0xDA60B8D0u,
  0x44042D73u,0x33031DE5u,0xAA0A4C5Fu,0xDD0D7CC9u,0x5005713Cu,0x270241AAu,
  0xBE0B1010u,0xC90C2086u,0x5768B525u,0x206F85B3u,0xB966D409u,0xCE61E49Fu,
  0x5EDEF90Eu,0x29D9C998u,0xB0D09822u,0xC7D7A8B4u,0x59B33D17u,0x2EB40D81u,
  0xB7BD5C3Bu,0xC0BA6CADu,0xEDB88320u,0x9ABFB3B6u,0x03B6E20Cu,0x74B1D29Au,
  0xEAD54739u,0x9DD277AFu,0x04DB2615u,0x73DC1683u,0xE3630B12u,0x94643B84u,
  0x0D6D6A3Eu,0x7A6A5AA8u,0xE40ECF0Bu,0x9309FF9Du,0x0A00AE27u,0x7D079EB1u,
  0xF00F9344u,0x8708A3D2u,0x1E01F268u,0x6906C2FEu,0xF762575Du,0x806567CBu,
  0x196C3671u,0x6E6B06E7u,0xFED41B76u,0x89D32BE0u,0x10DA7A5Au,0x67DD4ACCu,
  0xF9B9DF6Fu,0x8EBEEFF9u,0x17B7BE43u,0x60B08ED5u,0xD6D6A3E8u,0xA1D1937Eu,
  0x38D8C2C4u,0x4FDFF252u,0xD1BB67F1u,0xA6BC5767u,0x3FB506DDu,0x48B2364Bu,
  0xD80D2BDAu,0xAF0A1B4Cu,0x36034AF6u,0x41047A60u,0xDF60EFC3u,0xA867DF55u,
  0x316E8EEFu,0x4669BE79u,0xCB61B38Cu,0xBC66831Au,0x256FD2A0u,0x5268E236u,
  0xCC0C7795u,0xBB0B4703u,0x220216B9u,0x5505262Fu,0xC5BA3BBEu,0xB2BD0B28u,
  0x2BB45A92u,0x5CB36A04u,0xC2D7FFA7u,0xB5D0CF31u,0x2CD99E8Bu,0x5BDEAE1Du,
  0x9B64C2B0u,0xEC63F226u,0x756AA39Cu,0x026D930Au,0x9C0906A9u,0xEB0E363Fu,
  0x72076785u,0x05005713u,0x95BF4A82u,0xE2B87A14u,0x7BB12BAEu,0x0CB61B38u,
  0x92D28E9Bu,0xE5D5BE0Du,0x7CDCEFB7u,0x0BDBDF21u,0x86D3D2D4u,0xF1D4E242u,
  0x68DDB3F8u,0x1FDA836Eu,0x81BE16CDu,0xF6B9265Bu,0x6FB077E1u,0x18B74777u,
  0x88085AE6u,0xFF0F6A70u,0x66063BCAu,0x11010B5Cu,0x8F659EFFu,0xF862AE69u,
  0x616BFFD3u,0x166CCF45u,0xA00AE278u,0xD70DD2EEu,0x4E048354u,0x3903B3C2u,
  0xA7672661u,0xD06016F7u,0x4969474Du,0x3E6E77DBu,0xAED16A4Au,0xD9D65ADCu,
  0x40DF0B66u,0x37D83BF0u,0xA9BCAE53u,0xDEBB9EC5u,0x47B2CF7Fu,0x30B5FFE9u,
  0xBDBDF21Cu,0xCABAC28Au,0x53B39330u,0x24B4A3A6u,0xBAD03605u,0xCDD70693u,
  0x54DE5729u,0x23D967BFu,0xB3667A2Eu,0xC4614AB8u,0x5D681B02u,0x2A6F2B94u,
  0xB40BBE37u,0xC30C8EA1u,0x5A05DF1Bu,0x2D02EF8Du
};

constexpr uint32_t kMaxList = 1u << 20;       // reject absurd lengths
constexpr uint32_t kMaxString = 1u << 20;
constexpr uint32_t kMaxPayload = 1u << 30;
}  // namespace

void Crc32::update(const uint8_t* data, size_t len) noexcept {
  for (size_t i = 0; i < len; ++i) {
    crc_ = kCrcTable[(crc_ ^ data[i]) & 0xFFu] ^ (crc_ >> 8);
  }
}

uint32_t crc32(const uint8_t* data, size_t len) noexcept {
  Crc32 c;
  c.update(data, len);
  return c.value();
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------
void BinarySink::u8(uint8_t v) { buf_.push_back(v); }
void BinarySink::u16(uint16_t v) {
  buf_.push_back(static_cast<uint8_t>(v & 0xFF));
  buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void BinarySink::u32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void BinarySink::u64(uint64_t v) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void BinarySink::i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
void BinarySink::f64(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  u64(bits);
}
void BinarySink::bytes(const uint8_t* p, size_t n) {
  buf_.insert(buf_.end(), p, p + n);
}
void BinarySink::str(std::string_view s) {
  if (s.size() > kMaxString) throw value_error("string too long to persist");
  u32(static_cast<uint32_t>(s.size()));
  bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------
void BinarySource::need(size_t n) {
  if (remaining() < n) throw value_error("truncated binary stream");
}

uint8_t BinarySource::u8() {
  need(1);
  return *p_++;
}
uint16_t BinarySource::u16() {
  need(2);
  uint16_t v = static_cast<uint16_t>(p_[0]) | (static_cast<uint16_t>(p_[1]) << 8);
  p_ += 2;
  return v;
}
uint32_t BinarySource::u32() {
  need(4);
  uint32_t v = static_cast<uint32_t>(p_[0]) | (static_cast<uint32_t>(p_[1]) << 8) |
               (static_cast<uint32_t>(p_[2]) << 16) | (static_cast<uint32_t>(p_[3]) << 24);
  p_ += 4;
  return v;
}
uint64_t BinarySource::u64() {
  need(8);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(p_[i]) << (8 * i));
  p_ += 8;
  return v;
}
int64_t BinarySource::i64() { return static_cast<int64_t>(u64()); }
double BinarySource::f64() {
  uint64_t bits = u64();
  double v;
  std::memcpy(&v, &bits, sizeof(v));
  if (!(v == v) || v == std::numeric_limits<double>::infinity() ||
      v == -std::numeric_limits<double>::infinity())
    throw value_error("persisted non-finite double");
  return v;
}
void BinarySource::bytes(uint8_t* out, size_t n) {
  need(n);
  std::memcpy(out, p_, n);
  p_ += n;
}
std::string BinarySource::str() {
  uint32_t n = u32();
  if (n > kMaxString) throw value_error("malformed string length");
  need(n);
  std::string s(reinterpret_cast<const char*>(p_), n);
  p_ += n;
  return s;
}
std::string_view BinarySource::raw(size_t n) {
  need(n);
  std::string_view s(reinterpret_cast<const char*>(p_), n);
  p_ += n;
  return s;
}

// ---------------------------------------------------------------------------
// Section-based snapshot serialisation
// ---------------------------------------------------------------------------
namespace {
enum : uint8_t {
  KPolicy = 1,
  KCoord = 2,
  KResources = 3,
  KPaths = 4,
  KFlows = 5,
  KReservations = 6,
  KTenants = 7,
  KWorkloads = 8,
  KResourceState = 9,
};

void write_section(BinarySink& out, uint8_t tag, const std::vector<uint8_t>& body) {
  out.u8(tag);
  out.u32(static_cast<uint32_t>(body.size()));
  out.bytes(body.data(), body.size());
}

bool valid_flow_state(uint32_t v) { return v <= 8; }
bool valid_latency_class(uint32_t v) { return v <= 2; }
bool valid_reservation_state(uint32_t v) { return v <= 5; }
bool valid_resource_class(uint32_t v) { return v <= 7; }
bool valid_direction(uint32_t v) { return v <= 1; }
bool valid_health(uint32_t v) { return v <= 2; }

void write_capacity_field(BinarySink& s, const Capacity& c) { s.f64(c.value()); }
Capacity read_capacity_field(BinarySource& s) { return Capacity::make(s.f64()); }

void write_policy(BinarySink& s, const PolicyConfig& p) {
  s.f64(p.base_priority_weight);
  s.f64(p.latency_sensitive_weight);
  s.f64(p.throughput_weight);
  s.f64(p.best_effort_weight);
  s.f64(p.deadline_weight);
  s.f64(p.deadline_slack_seconds);
  s.f64(p.starvation_weight);
  s.f64(p.starvation_slow_seconds);
  s.f64(p.fairness_gain);
  s.f64(p.reservation_honour);
  s.i64(p.max_retries);
  s.u8(p.defer_if_min_infeasible ? 1 : 0);
  s.u8(p.reject_if_no_slack ? 1 : 0);
  s.u32(p.max_admitted);
  s.u8(p.deterministic_ties ? 1 : 0);
  s.f64(p.fairness_window_ms);
}

PolicyConfig read_policy(BinarySource& s) {
  PolicyConfig p;
  p.base_priority_weight = s.f64();
  p.latency_sensitive_weight = s.f64();
  p.throughput_weight = s.f64();
  p.best_effort_weight = s.f64();
  p.deadline_weight = s.f64();
  p.deadline_slack_seconds = s.f64();
  p.starvation_weight = s.f64();
  p.starvation_slow_seconds = s.f64();
  p.fairness_gain = s.f64();
  p.reservation_honour = s.f64();
  p.max_retries = static_cast<int>(s.i64());
  p.defer_if_min_infeasible = s.u8() != 0;
  p.reject_if_no_slack = s.u8() != 0;
  p.max_admitted = s.u32();
  p.deterministic_ties = s.u8() != 0;
  p.fairness_window_ms = s.f64();
  return p;
}

void write_resource_spec(BinarySink& s, const ResourceSpec& r) {
  s.id(r.id);
  s.u32(static_cast<uint32_t>(r.class_));
  s.str(r.source);
  s.str(r.destination);
  s.u32(static_cast<uint32_t>(r.direction));
  write_capacity_field(s, r.nominal);
  s.id(r.generation);
  s.u8(r.enabled ? 1 : 0);
}

ResourceSpec read_resource_spec(BinarySource& s) {
  ResourceSpec r;
  r.id = s.id<ResourceId>();
  uint32_t cls = s.u32();
  if (!valid_resource_class(cls)) throw value_error("invalid resource class");
  r.class_ = static_cast<ResourceClass>(cls);
  r.source = s.str();
  r.destination = s.str();
  uint32_t dir = s.u32();
  if (!valid_direction(dir)) throw value_error("invalid resource direction");
  r.direction = static_cast<Directionality>(dir);
  r.nominal = read_capacity_field(s);
  r.generation = s.id<CapacityGeneration>();
  r.enabled = s.u8() != 0;
  return r;
}

void write_path(BinarySink& s, const Path& p) {
  s.id(p.id);
  s.id(p.path_generation);
  s.u32(static_cast<uint32_t>(p.hops.size()));
  for (const PathHop& h : p.hops) {
    s.id(h.resource);
    s.id(h.generation);
  }
}

Path read_path(BinarySource& s) {
  Path p;
  p.id = s.id<PathId>();
  p.path_generation = s.id<FlowGeneration>();
  uint32_t n = s.u32();
  if (n > 256) throw value_error("path has too many hops");
  p.hops.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    PathHop h;
    h.resource = s.id<ResourceId>();
    h.generation = s.id<CapacityGeneration>();
    p.hops.push_back(h);
  }
  return p;
}

void write_flow_spec(BinarySink& s, const FlowSpec& f) {
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
  write_capacity_field(s, f.requested_min);
  write_capacity_field(s, f.requested_preferred);
  write_capacity_field(s, f.requested_max);
  s.u64(f.burst_bytes);
  s.u8(f.preemptible ? 1 : 0);
  s.u8(f.resumable ? 1 : 0);
}

FlowSpec read_flow_spec(BinarySource& s) {
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
  uint32_t dir = s.u32();
  if (!valid_direction(dir)) throw value_error("invalid flow direction");
  f.direction = static_cast<Directionality>(dir);
  f.priority = static_cast<int>(s.i64());
  uint32_t lc = s.u32();
  if (!valid_latency_class(lc)) throw value_error("invalid latency class");
  f.latency_class = static_cast<LatencyClass>(lc);
  f.deadline_seconds = s.f64();
  f.requested_min = read_capacity_field(s);
  f.requested_preferred = read_capacity_field(s);
  f.requested_max = read_capacity_field(s);
  f.burst_bytes = s.u64();
  f.preemptible = s.u8() != 0;
  f.resumable = s.u8() != 0;
  return f;
}

void write_flow_snapshot(BinarySink& s, const FlowSnapshot& f) {
  write_flow_spec(s, f.spec);
  uint32_t st = static_cast<uint32_t>(f.state);
  s.u32(st);
  write_capacity_field(s, f.granted);
  s.f64(f.admitted_ms);
  s.f64(f.last_update_ms);
  s.u64(f.bytes_transferred);
  s.u64(f.burst_remaining);
  s.u64(f.retry_count);
  s.f64(f.starvation_ms);
  s.u8(f.reservation ? 1 : 0);
  if (f.reservation) s.id(*f.reservation);
  s.str(f.last_error);
}

FlowSnapshot read_flow_snapshot(BinarySource& s) {
  FlowSnapshot f;
  f.spec = read_flow_spec(s);
  uint32_t st = s.u32();
  if (!valid_flow_state(st)) throw value_error("invalid flow state");
  f.state = static_cast<FlowState>(st);
  f.granted = read_capacity_field(s);
  f.admitted_ms = s.f64();
  f.last_update_ms = s.f64();
  f.bytes_transferred = s.u64();
  f.burst_remaining = s.u64();
  f.retry_count = s.u64();
  f.starvation_ms = s.f64();
  bool has_res = s.u8() != 0;
  if (has_res) f.reservation = s.id<ReservationId>();
  f.last_error = s.str();
  return f;
}

void write_resource_allocation(BinarySink& s, const ResourceAllocation& a) {
  s.id(a.resource);
  write_capacity_field(s, a.allocated);
}

ResourceAllocation read_resource_allocation(BinarySource& s) {
  ResourceAllocation a;
  a.resource = s.id<ResourceId>();
  a.allocated = read_capacity_field(s);
  return a;
}

void write_reservation(BinarySink& s, const Reservation& r) {
  s.id(r.id);
  s.id(r.flow);
  s.id(r.attempt);
  s.id(r.flow_generation);
  s.id(r.path);
  s.u32(static_cast<uint32_t>(r.allocations.size()));
  for (const ResourceAllocation& a : r.allocations) write_resource_allocation(s, a);
  s.id(r.epoch);
  s.id(r.worker_boot);
  s.id(r.capacity_generation);
  uint32_t st = static_cast<uint32_t>(r.state);
  s.u32(st);
}

Reservation read_reservation(BinarySource& s) {
  Reservation r;
  r.id = s.id<ReservationId>();
  r.flow = s.id<FlowId>();
  r.attempt = s.id<AttemptId>();
  r.flow_generation = s.id<FlowGeneration>();
  r.path = s.id<PathId>();
  uint32_t n = s.u32();
  if (n > 256) throw value_error("reservation has too many allocations");
  r.allocations.reserve(n);
  for (uint32_t i = 0; i < n; ++i) r.allocations.push_back(read_resource_allocation(s));
  r.epoch = s.id<CoordinatorEpoch>();
  r.worker_boot = s.id<WorkerBootId>();
  r.capacity_generation = s.id<CapacityGeneration>();
  uint32_t st = s.u32();
  if (!valid_reservation_state(st)) throw value_error("invalid reservation state");
  r.state = static_cast<ReservationState>(st);
  return r;
}

void write_tenant_acc(BinarySink& s, const TenantAccounting& t) {
  s.id(t.tenant);
  s.f64(t.allocated_now);
  s.f64(t.served_bytes);
  s.u64(t.submitted);
  s.u64(t.completed);
  s.u64(t.failed);
  s.f64(t.weight);
}

TenantAccounting read_tenant_acc(BinarySource& s) {
  TenantAccounting t;
  t.tenant = s.id<TenantId>();
  t.allocated_now = s.f64();
  t.served_bytes = s.f64();
  t.submitted = s.u64();
  t.completed = s.u64();
  t.failed = s.u64();
  t.weight = s.f64();
  return t;
}

void write_workload_acc(BinarySink& s, const WorkloadAccounting& w) {
  s.id(w.workload);
  s.f64(w.allocated_now);
  s.f64(w.served_bytes);
  s.u64(w.submitted);
  s.u64(w.completed);
  s.u64(w.failed);
  s.f64(w.weight);
}

WorkloadAccounting read_workload_acc(BinarySource& s) {
  WorkloadAccounting w;
  w.workload = s.id<WorkloadId>();
  w.allocated_now = s.f64();
  w.served_bytes = s.f64();
  w.submitted = s.u64();
  w.completed = s.u64();
  w.failed = s.u64();
  w.weight = s.f64();
  return w;
}

void write_resource_snapshot(BinarySink& s, const ResourceSnapshot& r) {
  s.id(r.id);
  s.u32(static_cast<uint32_t>(r.class_));
  s.str(r.source);
  s.str(r.destination);
  s.u32(static_cast<uint32_t>(r.direction));
  write_capacity_field(s, r.nominal);
  write_capacity_field(s, r.measured);
  write_capacity_field(s, r.governed);
  write_capacity_field(s, r.reserved);
  write_capacity_field(s, r.allocated);
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

ResourceSnapshot read_resource_snapshot(BinarySource& s) {
  ResourceSnapshot r;
  r.id = s.id<ResourceId>();
  uint32_t cls = s.u32();
  if (!valid_resource_class(cls)) throw value_error("invalid resource class in snapshot");
  r.class_ = static_cast<ResourceClass>(cls);
  r.source = s.str();
  r.destination = s.str();
  uint32_t dir = s.u32();
  if (!valid_direction(dir)) throw value_error("invalid direction in snapshot");
  r.direction = static_cast<Directionality>(dir);
  r.nominal = read_capacity_field(s);
  r.measured = read_capacity_field(s);
  r.governed = read_capacity_field(s);
  r.reserved = read_capacity_field(s);
  r.allocated = read_capacity_field(s);
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
  uint32_t h = s.u32();
  if (!valid_health(h)) throw value_error("invalid resource health");
  r.health = static_cast<ResourceHealth>(h);
  r.enabled = s.u8() != 0;
  return r;
}
}  // namespace

std::vector<uint8_t> encode_snapshot(const GovernorSnapshot& snap) {
  BinarySink payload;

  {  // policy
    BinarySink body;
    write_policy(body, snap.policy);
    write_section(payload, KPolicy, body.data());
  }
  {  // coordinator metadata
    BinarySink body;
    body.id(snap.coordinator);
    body.id(snap.epoch);
    body.id(snap.capacity_generation);
    body.f64(snap.saved_at_ms);
    write_section(payload, KCoord, body.data());
  }
  {  // resources
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.resources.size()));
    for (const ResourceSpec& r : snap.resources) write_resource_spec(body, r);
    write_section(payload, KResources, body.data());
  }
  {  // paths
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.paths.size()));
    for (const Path& p : snap.paths) write_path(body, p);
    write_section(payload, KPaths, body.data());
  }
  {  // flows
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.flows.size()));
    for (const FlowSnapshot& f : snap.flows) write_flow_snapshot(body, f);
    write_section(payload, KFlows, body.data());
  }
  {  // reservations
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.reservations.size()));
    for (const Reservation& r : snap.reservations) write_reservation(body, r);
    write_section(payload, KReservations, body.data());
  }
  {  // tenants
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.tenants.size()));
    for (const auto& [tid, t] : snap.tenants) { (void)tid; write_tenant_acc(body, t); }
    write_section(payload, KTenants, body.data());
  }
  {  // workloads
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.workloads.size()));
    for (const auto& [wid, w] : snap.workloads) { (void)wid; write_workload_acc(body, w); }
    write_section(payload, KWorkloads, body.data());
  }
  {  // resource state
    BinarySink body;
    body.u32(static_cast<uint32_t>(snap.resource_state.size()));
    for (const auto& [rid, r] : snap.resource_state) { (void)rid; write_resource_snapshot(body, r); }
    write_section(payload, KResourceState, body.data());
  }

  BinarySink env;
  env.bytes(reinterpret_cast<const uint8_t*>(kEnvelopeMagic.data()), kEnvelopeMagic.size());
  env.u32(kFormatVersion);
  env.u64(payload.size());
  env.bytes(payload.data().data(), payload.data().size());
  env.u32(crc32(payload.data().data(), payload.data().size()));
  return env.data();
}

GovernorSnapshot decode_snapshot(const uint8_t* data, size_t n) {
  BinarySource s(data, n);
  if (n < kEnvelopeMagic.size() + 4 + 8 + 4)
    throw value_error("envelope too short");
  auto magic = s.raw(kEnvelopeMagic.size());
  if (magic != kEnvelopeMagic) throw value_error("bad magic: not a Bandwidth Governor state");
  uint32_t version = s.u32();
  if (version != kFormatVersion) throw value_error("incompatible state version");
  uint64_t payload_len = s.u64();
  if (payload_len > kMaxPayload) throw value_error("payload length exceeds bound");
  std::vector<uint8_t> payload(payload_len);
  s.bytes(payload.data(), payload_len);
  uint32_t stored_crc = s.u32();
  if (stored_crc != crc32(payload.data(), payload.size()))
    throw value_error("checksum mismatch: state corrupted");
  if (s.remaining() != 0) throw value_error("trailing garbage after payload");

  BinarySource sec(payload.data(), payload.size());
  GovernorSnapshot snap;
  snap.format_version = version;
  std::unordered_set<uint8_t> seen;
  bool present[10] = {};
  while (sec.remaining() > 0) {
    uint8_t tag = sec.u8();
    uint32_t len = sec.u32();
    if (len > sec.remaining()) throw value_error("section length exceeds payload");
    if (seen.count(tag)) throw value_error("duplicate section field");
    seen.insert(tag);
    std::string_view sv = sec.raw(len);
    std::string bodybuf(sv);
    BinarySource sub(reinterpret_cast<const uint8_t*>(bodybuf.data()), len);
    switch (tag) {
      case KPolicy:
        snap.policy = read_policy(sub);
        present[KPolicy] = true;
        break;
      case KCoord:
        snap.coordinator = sub.id<CoordinatorId>();
        snap.epoch = sub.id<CoordinatorEpoch>();
        snap.capacity_generation = sub.id<CapacityGeneration>();
        snap.saved_at_ms = sub.f64();
        present[KCoord] = true;
        break;
      case KResources: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("resource count exceeds bound");
        snap.resources.reserve(count);
        std::unordered_set<ResourceId> ids;
        for (uint32_t i = 0; i < count; ++i) {
          ResourceSpec r = read_resource_spec(sub);
          if (ids.count(r.id)) throw value_error("duplicate resource id");
          ids.insert(r.id);
          snap.resources.push_back(std::move(r));
        }
        present[KResources] = true;
        break;
      }
      case KPaths: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("path count exceeds bound");
        snap.paths.reserve(count);
        std::unordered_set<PathId> ids;
        for (uint32_t i = 0; i < count; ++i) {
          Path p = read_path(sub);
          if (ids.count(p.id)) throw value_error("duplicate path id");
          ids.insert(p.id);
          snap.paths.push_back(std::move(p));
        }
        present[KPaths] = true;
        break;
      }
      case KFlows: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("flow count exceeds bound");
        snap.flows.reserve(count);
        std::unordered_set<FlowId> ids;
        for (uint32_t i = 0; i < count; ++i) {
          FlowSnapshot f = read_flow_snapshot(sub);
          if (ids.count(f.spec.id)) throw value_error("duplicate flow id");
          ids.insert(f.spec.id);
          snap.flows.push_back(std::move(f));
        }
        present[KFlows] = true;
        break;
      }
      case KReservations: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("reservation count exceeds bound");
        snap.reservations.reserve(count);
        std::unordered_set<ReservationId> ids;
        for (uint32_t i = 0; i < count; ++i) {
          Reservation r = read_reservation(sub);
          if (ids.count(r.id)) throw value_error("duplicate reservation id");
          ids.insert(r.id);
          snap.reservations.push_back(std::move(r));
        }
        present[KReservations] = true;
        break;
      }
      case KTenants: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("tenant count exceeds bound");
        for (uint32_t i = 0; i < count; ++i) {
          TenantAccounting t = read_tenant_acc(sub);
          snap.tenants[t.tenant] = t;
        }
        present[KTenants] = true;
        break;
      }
      case KWorkloads: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("workload count exceeds bound");
        for (uint32_t i = 0; i < count; ++i) {
          WorkloadAccounting w = read_workload_acc(sub);
          snap.workloads[w.workload] = w;
        }
        present[KWorkloads] = true;
        break;
      }
      case KResourceState: {
        uint32_t count = sub.u32();
        if (count > kMaxList) throw value_error("resource state count exceeds bound");
        for (uint32_t i = 0; i < count; ++i) {
          ResourceSnapshot r = read_resource_snapshot(sub);
          snap.resource_state[r.id] = r;
        }
        present[KResourceState] = true;
        break;
      }
      default:
        throw value_error("unknown section tag");
    }
    if (sub.remaining() != 0) throw value_error("trailing garbage in section");
  }
  for (uint8_t tag = 1; tag <= 9; ++tag) {
    if (!present[tag]) throw value_error("missing required section");
  }
  return snap;
}

}  // namespace bg

