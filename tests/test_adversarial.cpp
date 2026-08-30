// Bandwidth Governor - adversarial (malformed input / boundaries) tests.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg_test.hpp"
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/resource.hpp"
#include "bg/governor.hpp"
#include "bg/wire.hpp"
#include "bg/persist.hpp"

#include <vector>

using namespace bg;

// ---- Capacity / identity boundaries ------------------------------------------
BG_TEST("adversarial: capacity boundary values") {
  CHECK(Capacity::is_valid_rate(0.0));
  CHECK(Capacity::is_valid_rate(kMaxRate));
  CHECK(!Capacity::is_valid_rate(kMaxRate * 1.0000001));
  CHECK(!Capacity::is_valid_rate(-kEpsilon));
  CHECK(Capacity::make(kMaxRate).value() == kMaxRate);
  CHECK(!Capacity::try_make(kMaxRate * 2.0).has_value());
  Capacity a = Capacity::make(kMaxRate * 0.5);
  a += a;
  CHECK(a.value() <= kMaxRate);
}

BG_TEST("adversarial: flow spec validation rejects impossible inputs") {
  FlowSpec s;
  s.id = FlowId(0, 1);
  s.tenant = TenantId(0, 1);
  s.workload = WorkloadId(0, 1);
  s.attempt = AttemptId(0, 1);
  s.generation = fresh_generation<FlowGeneration>(1);
  s.path = PathId(0, 1);
  s.byte_count = 0;
  REQUIRE(validate_flow_spec(s).has_value());
  s.byte_count = 1024;
  s.requested_min = Capacity::make(100.0);
  s.requested_preferred = Capacity::make(50.0);  // below min -> invalid
  REQUIRE(validate_flow_spec(s).has_value());
  s.requested_preferred = Capacity::make(200.0);
  s.requested_max = Capacity::make(150.0);       // below preferred -> invalid
  REQUIRE(validate_flow_spec(s).has_value());
  s.requested_max = Capacity::make(500.0);
  s.requested_preferred = Capacity::make(300.0);
  CHECK(!validate_flow_spec(s).has_value());
}

// ---- Wire frame malformation -------------------------------------------------
BG_TEST("adversarial: wire frame rejects bad magic") {
  auto body = payload::encode_flow_spec(FlowSpec{});
  auto frame = encode_frame(WireType::SubmitFlow, body);
  REQUIRE(frame.size() > 4);
  frame[0] ^= 0xFF;
  REQUIRE_THROWS_AS(decode_frame(frame.data(), frame.size()), value_error);
}

BG_TEST("adversarial: wire frame rejects bad checksum") {
  auto body = payload::encode_flow_spec(FlowSpec{});
  auto frame = encode_frame(WireType::SubmitFlow, body);
  REQUIRE(frame.size() > 8);
  frame[frame.size() - 1] ^= 0xFF;
  REQUIRE_THROWS_AS(decode_frame(frame.data(), frame.size()), value_error);
}

BG_TEST("adversarial: wire frame rejects truncation and trailing garbage") {
  auto body = payload::encode_flow_spec(FlowSpec{});
  auto frame = encode_frame(WireType::SubmitFlow, body);
  REQUIRE_THROWS_AS(decode_frame(frame.data(), frame.size() - 3), value_error);
  std::vector<uint8_t> extended = frame;
  extended.push_back(0xAA);
  REQUIRE_THROWS_AS(decode_frame(extended.data(), extended.size()), value_error);
}

// ---- State/authority ----------------------------------------------------------
BG_TEST("adversarial: reservation double-release is rejected (exactly-once)") {
  Governor g(GovernorConfig{PolicyConfig{}, 11ULL, false});
  g.set_auto_clock(false);
  g.advance_ms(0.0);
  ResourceSpec s;
  s.id = ResourceId(0, 1);
  s.class_ = ResourceClass::Pcie;
  s.nominal = Capacity::make(1e9);
  s.generation = fresh_generation<CapacityGeneration>(1);
  g.add_resource(s);
  Path p;
  p.id = PathId(0, 1);
  p.path_generation = fresh_generation<FlowGeneration>(1);
  p.hops.push_back({s.id, s.generation});
  g.add_path(p);
  FlowSpec f;
  f.id = FlowId(0, 1);
  f.tenant = TenantId(0, 1);
  f.workload = WorkloadId(0, 1);
  f.attempt = AttemptId(0, 1);
  f.generation = fresh_generation<FlowGeneration>(1);
  f.source = "n";
  f.destination = "n";
  f.path = p.id;
  f.byte_count = 1024;
  f.requested_min = Capacity::make(0.0);
  f.requested_preferred = Capacity::make(10.0);
  f.requested_max = Capacity::make(10.0);
  auto r = g.submit_flow(f);
  REQUIRE(r.admitted);
  REQUIRE(r.reservation.has_value());
  CHECK(g.release_reservation(r.reservation->id));
  CHECK(!g.release_reservation(r.reservation->id));
  g.cancel_flow(f.id, "teardown");
  g.tick();
  CHECK(g.accounting_at_zero());
}

// ---- Persistence corruption ---------------------------------------------------
BG_TEST("adversarial: snapshot decodes and rejects corruption via checksum") {
  Governor g(GovernorConfig{PolicyConfig{}, 13ULL, false});
  g.set_auto_clock(false);
  g.advance_ms(0.0);
  auto bytes = g.save();
  REQUIRE(bytes.size() > 0);
  auto snap = decode_snapshot(bytes.data(), bytes.size());
  CHECK(snap.format_version == kStateFormatVersion);
  std::vector<uint8_t> corrupt = bytes;
  corrupt[corrupt.size() / 2] ^= 0x5A;
  REQUIRE_THROWS_AS(decode_snapshot(corrupt.data(), corrupt.size()), value_error);
}
