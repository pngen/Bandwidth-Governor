// Bandwidth Governor - downstream find_package consumer.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include <bg/governor.hpp>
#include <cstdio>

int main() {
  bg::Governor g(bg::GovernorConfig{});
  bg::ResourceSpec s;
  s.id = bg::ResourceId(0, 1);
  s.class_ = bg::ResourceClass::Pcie;
  s.source = "n";
  s.destination = "n";
  s.nominal = bg::Capacity::make(1e9);
  s.generation = bg::fresh_generation<bg::CapacityGeneration>(1);
  g.add_resource(s);
  bg::Path p;
  p.id = bg::PathId(0, 1);
  p.path_generation = bg::fresh_generation<bg::FlowGeneration>(1);
  p.hops.push_back({s.id, s.generation});
  g.add_path(p);

  bg::FlowSpec f;
  f.id = bg::FlowId(0, 1);
  f.tenant = bg::TenantId(0, 1);
  f.workload = bg::WorkloadId(0, 1);
  f.attempt = bg::AttemptId(0, 1);
  f.generation = bg::fresh_generation<bg::FlowGeneration>(1);
  f.source = "n";
  f.destination = "n";
  f.path = p.id;
  f.byte_count = 1 << 20;
  f.requested_min = bg::Capacity::make(0.0);
  f.requested_preferred = bg::Capacity::make(200e6);
  f.requested_max = bg::Capacity::make(200e6);

  auto r = g.submit_flow(f);
  if (!r.admitted) { std::printf("consumer: admission failed\n"); return 1; }
  g.tick();
  auto fs = g.flow(f.id);
  if (!fs || fs->state != bg::FlowState::Running) { std::printf("consumer: flow not running\n"); return 1; }
  std::printf("consumer: flow %s running at %.0f B/s\n", f.id.to_string().c_str(), fs->granted.value());
  return 0;
}
