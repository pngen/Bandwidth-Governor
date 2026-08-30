// Bandwidth Governor - core unit tests.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg_test.hpp"

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/reservation.hpp"
#include "bg/policy.hpp"
#include "bg/resource.hpp"
#include "bg/governor.hpp"

#include <cmath>

using namespace bg;

// ---- Capacity ----------------------------------------------------------------
BG_TEST("capacity: valid rates accepted") {
  Capacity c = Capacity::make(1.0e6);
  CHECK(c.value() == 1.0e6);
  CHECK(c.valid());
  CHECK(Capacity{}.is_zero());
}

BG_TEST("capacity: invalid rates rejected") {
  REQUIRE_THROWS_AS(Capacity::make(-1.0), value_error);
  REQUIRE_THROWS_AS(Capacity::make(std::nan("")), value_error);
  REQUIRE_THROWS_AS(Capacity::make(std::numeric_limits<double>::infinity()), value_error);
  REQUIRE_THROWS_AS(Capacity::make(kMaxRate * 2.0), value_error);
}

BG_TEST("capacity: try_make is non-throwing") {
  CHECK(!Capacity::try_make(-1.0).has_value());
  CHECK(!Capacity::try_make(std::nan("")).has_value());
  CHECK(Capacity::try_make(5.0).has_value());
}

// ---- Identity ----------------------------------------------------------------
BG_TEST("identity: distinct typed ids and formatting") {
  IdGen g(42);
  FlowId a = g.next<FlowId>();
  FlowId b = g.next<FlowId>();
  CHECK(a != b);
  CHECK(a.to_string().size() == 32);
  CHECK(FlowId::type_name() == "FlowId");
  CHECK(a.is_null() == false);
  CHECK(FlowId{}.is_null());
}

BG_TEST("identity: generation bump never yields null") {
  FlowGeneration g = fresh_generation<FlowGeneration>(7);
  FlowGeneration g2 = next_generation(g);
  CHECK(!g2.is_null());
  CHECK(g2 != g);
}

// ---- Flow lifecycle ------------------------------------------------------------
BG_TEST("flow: valid transitions") {
  CHECK(flow_transition(FlowState::Created, FlowState::Queued).has_value());
  CHECK(flow_transition(FlowState::Queued, FlowState::Reserved).has_value());
  CHECK(flow_transition(FlowState::Reserved, FlowState::Running).has_value());
  CHECK(flow_transition(FlowState::Running, FlowState::Completed).has_value());
  CHECK(flow_transition(FlowState::Running, FlowState::Throttled).has_value());
  CHECK(flow_transition(FlowState::Throttled, FlowState::Running).has_value());
  CHECK(flow_transition(FlowState::Preempted, FlowState::Queued).has_value());
}

BG_TEST("flow: invalid transitions rejected") {
  CHECK(!flow_transition(FlowState::Completed, FlowState::Running).has_value());
  CHECK(!flow_transition(FlowState::Failed, FlowState::Queued).has_value());
  CHECK(!flow_transition(FlowState::Cancelled, FlowState::Completed).has_value());
  CHECK(!flow_transition(FlowState::Created, FlowState::Running).has_value());
}

// ---- Reservations ---------------------------------------------------------------
BG_TEST("reservation: exactly once release") {
  CHECK(reservation_transition(ReservationState::Active, ReservationState::Released).has_value());
  CHECK(reservation_transition(ReservationState::Released, ReservationState::Released).has_value());
  CHECK(!reservation_transition(ReservationState::Released, ReservationState::Active).has_value());
  CHECK(!reservation_transition(ReservationState::Released, ReservationState::Cancelled).has_value());
}

// ---- Weighted allocation ---------------------------------------------------------
BG_TEST("allocator: equal weight fair share") {
  std::vector<AllocEntry> e = {
    {FlowId(0, 1), 1.0, 0.0, 100.0, 100.0},
    {FlowId(0, 2), 1.0, 0.0, 100.0, 100.0},
  };
  AllocResult r = allocate_weighted(e, 200.0);
  CHECK(std::abs(r.granted[0] - 100.0) < 1e-6);
  CHECK(std::abs(r.granted[1] - 100.0) < 1e-6);
}

BG_TEST("allocator: priority weighting") {
  std::vector<AllocEntry> e = {
    {FlowId(0, 1), 4.0, 0.0, 1000.0, 1000.0},  // high weight
    {FlowId(0, 2), 1.0, 0.0, 1000.0, 1000.0},
  };
  AllocResult r = allocate_weighted(e, 500.0);
  // water-fill: high weight gets 4x when combined capacity is a bottleneck
  CHECK(r.granted[0] > r.granted[1]);
  CHECK(std::abs(r.granted[0] + r.granted[1] - 500.0) < 1e-6);
}

BG_TEST("allocator: minimum guarantee honoured") {
  std::vector<AllocEntry> e = {
    {FlowId(0, 1), 1.0, 80.0, 200.0, 200.0},
    {FlowId(0, 2), 1.0, 80.0, 200.0, 200.0},
  };
  AllocResult r = allocate_weighted(e, 300.0);
  CHECK(r.granted[0] >= 79.0);
  CHECK(r.granted[1] >= 79.0);
  CHECK(std::abs(r.granted[0] + r.granted[1] - 300.0) < 1e-6);
}

BG_TEST("allocator: capped by max") {
  std::vector<AllocEntry> e = {
    {FlowId(0, 1), 1.0, 0.0, 1000.0, 50.0},
    {FlowId(0, 2), 1.0, 0.0, 1000.0, 50.0},
  };
  AllocResult r = allocate_weighted(e, 500.0);
  CHECK(r.granted[0] <= 50.0);
  CHECK(r.granted[1] <= 50.0);
  CHECK(std::abs(r.granted[0] + r.granted[1] - 100.0) < 1e-6);
}

BG_TEST("allocator: saturation flag") {
  std::vector<AllocEntry> e = {
    {FlowId(0, 1), 1.0, 0.0, 1000.0, 1000.0},
  };
  AllocResult r = allocate_weighted(e, 100.0);
  CHECK(r.saturated);
  CHECK(std::abs(r.granted[0] - 100.0) < 1e-6);
}

// ---- Governor: basic admission and accounting ------------------------------------
namespace {
struct Fixture {
  Governor g;
  ResourceId res;
  PathId path;
  Fixture() : g(GovernorConfig{.id_salt = 1234, .auto_clock = false}) {
    ResourceSpec s;
    s.id = ResourceId(0, 1);
    s.class_ = ResourceClass::Pcie;
    s.source = "gpu0";
    s.destination = "host";
    s.nominal = Capacity::make(1.0e9);
    s.generation = fresh_generation<CapacityGeneration>(1);
    res = g.add_resource(s);
    Path p;
    p.id = PathId(0, 1);
    p.path_generation = fresh_generation<FlowGeneration>(1);
    p.hops.push_back({res, s.generation});
    path = g.add_path(p);
  }
};

FlowSpec make_flow(const Fixture& f, TenantId t, WorkloadId w, double min, double pref, double max,
                   size_t n) {
  FlowSpec s;
  s.id = FlowId(0, static_cast<uint64_t>(n + 1));
  s.tenant = t;
  s.workload = w;
  s.attempt = AttemptId(0, static_cast<uint64_t>(n + 1));
  s.generation = fresh_generation<FlowGeneration>(static_cast<uint64_t>(n + 1));
  s.source = "a";
  s.destination = "b";
  s.path = f.path;
  s.byte_count = 1024 * 1024;
  s.requested_min = Capacity::make(min);
  s.requested_preferred = Capacity::make(pref);
  s.requested_max = Capacity::make(max);
  return s;
}
}  // namespace

BG_TEST("governor: submit, grant, account, release to zero") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t(0, 10);
  WorkloadId w(0, 20);
  FlowSpec s = make_flow(f, t, w, 0.0, 250.0e6, 250.0e6, 1);
  AdmissionResult r = f.g.submit_flow(s);
  CHECK(r.admitted);
  CHECK(r.reservation.has_value());
  f.g.tick();
  auto fs = f.g.flow(s.id);
  REQUIRE(fs.has_value());
  CHECK(fs->state == FlowState::Running);
  CHECK(std::abs(fs->granted.value() - 250.0e6) < 1e3);
  // complete the flow
  CHECK(f.g.report_completion(s.id, s.attempt, s.generation,
                              WorkerBootId{}, s.byte_count));
  CHECK(f.g.accounting_at_zero());
}

BG_TEST("governor: reservation pressure and exactly-once release") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t(0, 10);
  WorkloadId w(0, 20);
  FlowSpec s = make_flow(f, t, w, 0.0, 250.0e6, 250.0e6, 2);
  AdmissionResult r = f.g.submit_flow(s);
  REQUIRE(r.reservation.has_value());
  ReservationId rid = r.reservation->id;
  CHECK(f.g.release_reservation(rid));
  // second release must fail (exactly-once)
  CHECK(!f.g.release_reservation(rid));
}

BG_TEST("governor: admission defers when capacity is insufficient") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t(0, 30);
  WorkloadId w(0, 40);
  // request min beyond capacity => defer
  FlowSpec s = make_flow(f, t, w, 2.0e9, 2.0e9, 2.0e9, 3);
  AdmissionResult r = f.g.submit_flow(s);
  CHECK(!r.admitted);
  CHECK(r.decision.kind == DecisionKind::Defer || r.decision.kind == DecisionKind::Reject);
}

BG_TEST("governor: shared link contention saturates") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t1(0, 50);
  TenantId t2(0, 51);
  WorkloadId w(0, 60);
  FlowSpec a = make_flow(f, t1, w, 0.0, 600.0e6, 600.0e6, 4);
  FlowSpec b = make_flow(f, t2, w, 0.0, 600.0e6, 600.0e6, 5);
  AdmissionResult ra = f.g.submit_flow(a);
  AdmissionResult rb = f.g.submit_flow(b);
  REQUIRE(ra.admitted);
  REQUIRE(rb.admitted);
  f.g.tick();
  auto fa = f.g.flow(a.id);
  auto fb = f.g.flow(b.id);
  REQUIRE(fa.has_value());
  REQUIRE(fb.has_value());
  // both running but shared capacity of 1e9 split; each < 600e6
  CHECK(fa->state == FlowState::Running);
  CHECK(fb->state == FlowState::Running);
  double sum = fa->granted.value() + fb->granted.value();
  // The shared 1 GB/s link is saturated: the two flows together use the full
  // link within a small floating-point tolerance.
  CHECK(std::abs(sum - 1.0e9) < 2.0e6);
  CHECK(fa->granted.value() < 600.0e6 + 1.0);
  CHECK(fb->granted.value() < 600.0e6 + 1.0);
  auto res = f.g.resource(f.res);
  REQUIRE(res.has_value());
  CHECK(res->saturated);
}

BG_TEST("governor: disjoint paths are independent") {
  Fixture f;
  f.g.advance_ms(0.0);
  // a second resource and path
  ResourceSpec s2;
  s2.id = ResourceId(0, 2);
  s2.class_ = ResourceClass::Nvlink;
  s2.source = "gpu0";
  s2.destination = "gpu1";
  s2.nominal = Capacity::make(1.0e9);
  s2.generation = fresh_generation<CapacityGeneration>(2);
  auto res2 = f.g.add_resource(s2);
  Path p2;
  p2.id = PathId(0, 2);
  p2.path_generation = fresh_generation<FlowGeneration>(2);
  p2.hops.push_back({res2, s2.generation});
  auto path2 = f.g.add_path(p2);

  FlowSpec a = make_flow(f, TenantId(0, 50), WorkloadId(0, 60), 0.0, 600.0e6, 600.0e6, 4);
  FlowSpec b = a;
  b.id = FlowId(0, 6);
  b.attempt = AttemptId(0, 6);
  b.generation = fresh_generation<FlowGeneration>(6);
  b.path = path2;
  AdmissionResult ra = f.g.submit_flow(a);
  AdmissionResult rb = f.g.submit_flow(b);
  REQUIRE(ra.admitted);
  REQUIRE(rb.admitted);
  f.g.tick();
  auto fa = f.g.flow(a.id);
  auto fb = f.g.flow(b.id);
  REQUIRE(fa.has_value());
  REQUIRE(fb.has_value());
  // each path has its own 1e9 capacity -> each flow gets full 600e6
  CHECK(std::abs(fa->granted.value() - 600.0e6) < 1e3);
  CHECK(std::abs(fb->granted.value() - 600.0e6) < 1e3);
}

BG_TEST("governor: distributed dispatch promotes flow to running") {
  Fixture f;
  f.g.advance_ms(0.0);
  WorkerRegistration wr;
  wr.worker = WorkerId(0, 1);
  wr.boot = WorkerBootId(0, 10);
  ResourceSpec inv;
  inv.id = f.res;
  inv.class_ = ResourceClass::Pcie;
  inv.source = "n";
  inv.destination = "n";
  inv.nominal = Capacity::make(1e9);
  inv.generation = fresh_generation<CapacityGeneration>(1);
  wr.inventory = {inv};
  f.g.register_worker(wr);
  TenantId t(0, 99);
  WorkloadId w(0, 98);
  FlowSpec s = make_flow(f, t, w, 0.0, 300.0e6, 300.0e6, 40);
  AdmissionResult r = f.g.submit_flow(s);
  REQUIRE(r.admitted);
  f.g.advance_ms(1.0);
  f.g.tick();
  auto fs = f.g.flow(s.id);
  REQUIRE(fs.has_value());
  CHECK(fs->state == FlowState::Running);
  CHECK(fs->assigned_worker.has_value());
  // complete via the worker with the correct boot, then verify accounting zero.
  CHECK(f.g.report_completion(s.id, s.attempt, s.generation, WorkerBootId(0, 10), s.byte_count));
  CHECK(f.g.accounting_at_zero());
}

BG_TEST("governor: stale authority completion is rejected") {
  Fixture f;
  f.g.advance_ms(0.0);
  WorkerRegistration wr;
  wr.worker = WorkerId(0, 1);
  wr.boot = WorkerBootId(0, 10);
  ResourceSpec inv;
  inv.id = f.res;
  inv.class_ = ResourceClass::Pcie;
  inv.source = "n";
  inv.destination = "n";
  inv.nominal = Capacity::make(1e9);
  inv.generation = fresh_generation<CapacityGeneration>(1);
  wr.inventory = {inv};
  f.g.register_worker(wr);
  TenantId t(0, 97);
  WorkloadId w(0, 96);
  FlowSpec s = make_flow(f, t, w, 0.0, 300.0e6, 300.0e6, 41);
  AdmissionResult r = f.g.submit_flow(s);
  REQUIRE(r.admitted);
  f.g.advance_ms(1.0);
  f.g.tick();
  auto fs = f.g.flow(s.id);
  REQUIRE(fs.has_value());
  CHECK(fs->state == FlowState::Running);
  // stale boot completion must be rejected and mutate nothing.
  CHECK(!f.g.report_completion(s.id, s.attempt, s.generation, WorkerBootId(0, 999), s.byte_count));
  auto fs2 = f.g.flow(s.id);
  REQUIRE(fs2.has_value());
  CHECK(fs2->state == FlowState::Running);
  // stale attempt must be rejected.
  CHECK(!f.g.report_completion(s.id, AttemptId(0, 999), s.generation, WorkerBootId(0, 10), s.byte_count));
  // restart the worker with a fresh boot: flow must be rolled (new attempt) and
  // must NOT be completed by the stale completion.
  WorkerBootId newboot(0, 11);
  WorkerRegistration wr2;
  wr2.worker = WorkerId(0, 1);
  wr2.boot = newboot;
  wr2.inventory = {inv};
  f.g.register_worker(wr2);
  f.g.advance_ms(1.0);
  f.g.tick();
  auto fs3 = f.g.flow(s.id);
  REQUIRE(fs3.has_value());
  std::printf("  stale_test: state=%s attempt_diff=%d assigned=%d\n",
              flow_state_name(fs3->state), (int)(fs3->spec.attempt != s.attempt),
              (int)fs3->assigned_worker.has_value());
  CHECK(fs3->state == FlowState::Running);
  CHECK(fs3->spec.attempt != s.attempt);  // retry got a new attempt id
  // complete on the fresh boot with the new attempt.
  bool comp_ok = f.g.report_completion(s.id, fs3->spec.attempt, fs3->spec.generation, newboot, s.byte_count);
  std::printf("  stale_test: comp_ok=%d\n", (int)comp_ok);
  CHECK(comp_ok);
  CHECK(f.g.accounting_at_zero());
}

// ---- Persistence -----------------------------------------------------------------
BG_TEST("persistence: round-trip and accounting") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t(0, 70);
  WorkloadId w(0, 80);
  FlowSpec s = make_flow(f, t, w, 0.0, 100.0e6, 100.0e6, 8);
  AdmissionResult r = f.g.submit_flow(s);
  REQUIRE(r.admitted);
  f.g.tick();
  std::vector<uint8_t> bytes = f.g.save();
  CHECK(!bytes.empty());
  Governor g2;
  g2.load(bytes.data(), bytes.size());
  auto fs = g2.flow(s.id);
  REQUIRE(fs.has_value());
  CHECK(fs->spec.id == s.id);
  CHECK(fs->state == FlowState::Running);
  CHECK(std::abs(fs->granted.value() - 100.0e6) < 1e3);
  // complete in recovered instance and verify accounting reaches zero
  CHECK(g2.report_completion(s.id, s.attempt, s.generation, WorkerBootId{}, s.byte_count));
  CHECK(g2.accounting_at_zero());
}

BG_TEST("persistence: corruption rejected") {
  Fixture f;
  f.g.advance_ms(0.0);
  TenantId t(0, 90);
  WorkloadId w(0, 91);
  FlowSpec s = make_flow(f, t, w, 0.0, 100.0e6, 100.0e6, 9);
  AdmissionResult r = f.g.submit_flow(s);
  REQUIRE(r.admitted);
  std::vector<uint8_t> bytes = f.g.save();
  REQUIRE(bytes.size() > 16);
  bytes[bytes.size() - 5] ^= 0xFF;  // corrupt a payload byte
  Governor g2;
  REQUIRE_THROWS_AS(g2.load(bytes.data(), bytes.size()), value_error);
}

BG_TEST("persistence: truncation rejected") {
  Fixture f;
  f.g.advance_ms(0.0);
  std::vector<uint8_t> bytes = f.g.save();
  REQUIRE(bytes.size() > 12);
  Governor g2;
  REQUIRE_THROWS_AS(g2.load(bytes.data(), 10), value_error);
}

BG_TEST("persistence: trailing garbage rejected") {
  Fixture f;
  f.g.advance_ms(0.0);
  std::vector<uint8_t> bytes = f.g.save();
  std::vector<uint8_t> extended = bytes;
  extended.push_back(0xAB);
  extended.push_back(0xCD);
  Governor g2;
  REQUIRE_THROWS_AS(g2.load(extended.data(), extended.size()), value_error);
}
