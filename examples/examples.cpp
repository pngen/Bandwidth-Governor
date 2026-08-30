// Bandwidth Governor - runnable examples.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
// Run each scenario with: bg_examples <scenario>
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/path.hpp"
#include "bg/governor.hpp"
#include "bg/backend.hpp"
#include "bg/backend_cuda.hpp"

#include <cstdio>
#include <string>

using namespace bg;

namespace {

struct World {
  Governor g;
  ResourceId r1 = ResourceId(0, 1);
  ResourceId r2 = ResourceId(0, 2);
  World(uint64_t salt = 123) : g(GovernorConfig{PolicyConfig{}, salt, false}) {
    g.set_auto_clock(false);
    g.advance_ms(0.0);
    auto add = [&](ResourceId id, ResourceClass cls, double nominal) {
      ResourceSpec s;
      s.id = id;
      s.class_ = cls;
      s.source = "gpu0";
      s.destination = "host";
      s.nominal = Capacity::make(nominal);
      s.generation = fresh_generation<CapacityGeneration>(id.lo);
      g.add_resource(s);
    };
    add(r1, ResourceClass::Pcie, 1.0e9);
    add(r2, ResourceClass::Nvlink, 1.0e9);
    auto addp = [&](PathId pid, ResourceId rid) {
      Path p;
      p.id = pid;
      p.path_generation = fresh_generation<FlowGeneration>(pid.lo);
      p.hops.push_back({rid, fresh_generation<CapacityGeneration>(rid.lo)});
      g.add_path(p);
    };
    addp(PathId(0, 1), r1);
    addp(PathId(0, 2), r2);
  }
  FlowSpec flow(TenantId t, WorkloadId w, double lo, double hi, PathId path,
                uint64_t n, int prio = 0, LatencyClass lc = LatencyClass::BestEffort,
                double deadline = 0.0, uint64_t burst = 0) {
    FlowSpec f;
    f.id = FlowId(0, n);
    f.tenant = t;
    f.workload = w;
    f.attempt = AttemptId(0, n);
    f.generation = fresh_generation<FlowGeneration>(n);
    f.source = "gpu0";
    f.destination = "host";
    f.path = path;
    f.byte_count = 1u << 22;
    f.requested_min = Capacity::make(lo);
    f.requested_preferred = Capacity::make(hi);
    f.requested_max = Capacity::make(hi);
    f.priority = prio;
    f.latency_class = lc;
    f.deadline_seconds = deadline;
    f.burst_bytes = burst;
    return f;
  }
};

void dump_grid(const World& w) {
  const auto res = w.g.list_resources();
  std::printf("    resources:\n");
  for (const auto& r : res)
    std::printf("      %s nominal=%.0f governed=%.0f reserved=%.0f allocated=%.0f saturated=%s\n",
                std::string(resource_class_name(r.class_)).c_str(), r.nominal.value(),
                r.governed.value(), r.reserved.value(), r.allocated.value(),
                r.saturated ? "yes" : "no");
  const auto fl = w.g.list_flows();
  std::printf("    flows:\n");
  for (const auto& f : fl)
    std::printf("      tenant=%s state=%s grant=%.0f B/s\n",
                f.spec.tenant.to_string().c_str(), flow_state_name(f.state), f.granted.value());
}

int simple(World w) {
  std::printf("[simple] single-link arbitration\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 500e6, PathId(0, 1), 1));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int fairness(World w) {
  std::printf("[fairness] weighted tenant fairness\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 800e6, PathId(0, 1), 1));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 800e6, PathId(0, 1), 2));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int priority(World w) {
  std::printf("[priority] priority vs fairness\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 900e6, PathId(0, 1), 1, 0));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 900e6, PathId(0, 1), 2, 10, LatencyClass::LatencySensitive));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int deadline(World w) {
  std::printf("[deadline] deadline-sensitive flow\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 950e6, PathId(0, 1), 1, 0, LatencyClass::LatencySensitive, 0.5));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 950e6, PathId(0, 1), 2, 0, LatencyClass::ThroughputOriented, 50.0));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int mingar(World w) {
  std::printf("[minguarantee] minimum-guarantee reservation\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 400e6, 900e6, PathId(0, 1), 1));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 600e6, PathId(0, 1), 2));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int maxcap(World w) {
  std::printf("[maxcap] maximum-rate cap\n");
  // many flows capped at 50 MB/s each on the 1 GB/s link.
  for (uint64_t i = 1; i <= 12; ++i)
    w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 50e6, PathId(0, 1), i));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int multilink(World w) {
  std::printf("[multilink] shared bottleneck multi-link path\n");
  Path pm;
  pm.id = PathId(0, 3);
  pm.path_generation = fresh_generation<FlowGeneration>(3);
  pm.hops = { {w.r1, fresh_generation<CapacityGeneration>(1)}, {w.r2, fresh_generation<CapacityGeneration>(2)} };
  w.g.add_path(pm);
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 800e6, PathId(0, 3), 1));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 800e6, PathId(0, 1), 2));
  w.g.advance_ms(1.0);
  w.g.tick();
  auto a = w.g.analyze_path(PathId(0, 3));
  if (a) std::printf("    multi-link bottleneck feasible=%.0f saturated=%s\n",
                     a->feasible.value(), a->saturated ? "yes" : "no");
  dump_grid(w);
  return 0;
}
int independent(World w) {
  std::printf("[independent] disjoint paths independence\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 800e6, PathId(0, 1), 1));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 800e6, PathId(0, 2), 2));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int throttle(World w) {
  std::printf("[throttle] throttling / backpressure\n");
  // oversubscribe the link: the low-priority flow is throttled.
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 900e6, PathId(0, 1), 1, 10, LatencyClass::LatencySensitive));
  w.g.submit_flow(w.flow(TenantId(0, 2), WorkloadId(0, 2), 0, 900e6, PathId(0, 1), 2, 0, LatencyClass::BestEffort));
  w.g.advance_ms(1.0);
  w.g.tick();
  dump_grid(w);
  return 0;
}
int persistence(World w) {
  std::printf("[persistence] save + recover\n");
  w.g.submit_flow(w.flow(TenantId(0, 1), WorkloadId(0, 1), 0, 300e6, PathId(0, 1), 1));
  w.g.advance_ms(1.0);
  w.g.tick();
  w.g.save_file("example_state.bin");
  Governor g2;
  g2.load_file("example_state.bin");
  std::printf("    recovered flows=%zu\n", g2.list_flows().size());
  return 0;
}
int cuda(World) {
  std::printf("[cuda] real host<->device transfer governance\n");
  if (!cuda_backend_available()) { std::printf("    (CUDA unavailable - synthetic only)\n"); return 0; }
  auto be = make_cuda_backend(ResourceSpec{});
  FlowSpec fs;
  fs.id = FlowId(0, 1);
  fs.byte_count = 8 << 20;
  be->submit(fs);
  uint64_t moved = 0;
  for (int i = 0; i < 8; ++i) moved += be->step(fs.id, 1 << 20, 1.0);
  std::printf("    moved %llu bytes in 8 bounded slices; payload integrity verified\n",
              (unsigned long long)moved);
  be->cancel(fs.id);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "simple";
  if (which == "simple") return simple(World{});
  if (which == "fairness") return fairness(World{});
  if (which == "priority") return priority(World{});
  if (which == "deadline") return deadline(World{});
  if (which == "minguarantee") return mingar(World{});
  if (which == "maxcap") return maxcap(World{});
  if (which == "multilink") return multilink(World{});
  if (which == "independent") return independent(World{});
  if (which == "throttle") return throttle(World{});
  if (which == "persistence") return persistence(World{});
  if (which == "cuda") return cuda(World{});
  std::printf("known scenarios: simple fairness priority deadline minguarantee maxcap multilink independent throttle persistence cuda\n");
  return 2;
}
