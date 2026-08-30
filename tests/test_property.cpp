// Bandwidth Governor - deterministic fixed-seed property testing.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg_test.hpp"
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/governor.hpp"

#include <random>

using namespace bg;

BG_TEST("property: fixed-seed random workloads preserve governor invariants") {
  // Deterministic sequence: the same seed always produces the same traffic.
  std::mt19937_64 rng(0xC0FFEEULL);
  Governor g(GovernorConfig{PolicyConfig{}, 77ULL, false});
  g.set_auto_clock(false);
  g.advance_ms(0.0);

  ResourceSpec s;
  s.id = ResourceId(0, 1);
  s.class_ = ResourceClass::Pcie;
  s.source = "n";
  s.destination = "n";
  s.nominal = Capacity::make(1.0e9);
  s.generation = fresh_generation<CapacityGeneration>(1);
  g.add_resource(s);
  Path p;
  p.id = PathId(0, 1);
  p.path_generation = fresh_generation<FlowGeneration>(1);
  p.hops.push_back({s.id, s.generation});
  g.add_path(p);

  const int kFlows = 120;
  int submitted = 0;
  for (int i = 0; i < kFlows; ++i) {
    FlowSpec f;
    f.id = FlowId(0, static_cast<uint64_t>(i + 1));
    f.tenant = TenantId(0, static_cast<uint64_t>((rng() % 4) + 1));
    f.workload = WorkloadId(0, static_cast<uint64_t>((rng() % 3) + 1));
    f.attempt = AttemptId(0, static_cast<uint64_t>(i + 1));
    f.generation = fresh_generation<FlowGeneration>(static_cast<uint64_t>(i + 1));
    f.source = "n";
    f.destination = "n";
    f.path = p.id;
    f.byte_count = static_cast<uint64_t>(rng() % 100000000ULL) + 1;
    double lo = static_cast<double>(rng() % 30000000ULL);
    double hi = lo + static_cast<double>(rng() % 60000000ULL) + 1.0;
    if (hi > 900.0e6) hi = 900.0e6;
    f.requested_min = Capacity::make(lo);
    f.requested_preferred = Capacity::make(hi);
    f.requested_max = Capacity::make(hi);
    f.priority = static_cast<int>(rng() % 16) - 8;
    f.latency_class = static_cast<LatencyClass>(rng() % 3);
    auto r = g.submit_flow(f);
    if (r.admitted) ++submitted;
    g.advance_ms(static_cast<double>(rng() % 3));
    g.tick();
  }
  CHECK(submitted > 0);

  // Invariants after all submissions + ticks:
  double allocated_on_link = 0.0;
  for (const auto& fs : g.list_flows()) {
    // granted is always within the flow's [min, max] envelope.
    double gr = fs.granted.value();
    CHECK(gr >= -1e-9);
    CHECK(gr <= fs.spec.requested_max.value() + 1e-6);
    CHECK(fs.granted.value() == fs.granted.value());  // not NaN
    if (fs.state == FlowState::Running || fs.state == FlowState::Throttled)
      allocated_on_link += gr;
  }
  // no allocation may exceed the governed link capacity.
  CHECK(allocated_on_link <= 1.0e9 * 1.001);

  // Cancel everything; accounting must return exactly to zero.
  for (const auto& fs : g.list_flows()) {
    g.advance_ms(1.0);
    g.cancel_flow(fs.spec.id, "property teardown");
    g.tick();
  }
  CHECK(g.accounting_at_zero());
}
