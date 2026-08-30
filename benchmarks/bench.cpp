// Bandwidth Governor - benchmark suite.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
// Reports completed operations per second (never misleading partial timings).
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/policy.hpp"
#include "bg/governor.hpp"
#include "bg/backend.hpp"
#include "bg/backend_cuda.hpp"
#include "bg/persist.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace bg;

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point a) {
  return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}

void fill_gov(Governor& g) {
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
}

FlowSpec flow_spec(uint64_t n, PathId path) {
  FlowSpec f;
  f.id = FlowId(0, n);
  f.tenant = TenantId(0, 1);
  f.workload = WorkloadId(0, 1);
  f.attempt = AttemptId(0, n);
  f.generation = fresh_generation<FlowGeneration>(n);
  f.source = "n";
  f.destination = "n";
  f.path = path;
  f.byte_count = 1u << 20;
  f.requested_min = Capacity::make(0.0);
  f.requested_preferred = Capacity::make(100.0e6);
  f.requested_max = Capacity::make(100.0e6);
  return f;
}
}  // namespace

int main() {
  std::printf("Bandwidth Governor benchmarks\n");

  // admission throughput
  {
    Governor g(GovernorConfig{PolicyConfig{}, 1ULL, false}); fill_gov(g);
    auto t0 = Clock::now();
    const int n = 4000;
    for (int i = 0; i < n; ++i) {
      FlowSpec f = flow_spec(static_cast<uint64_t>(i + 1), PathId(0, 1));
      g.submit_flow(f);
    }
    double ms = ms_since(t0);
    // note: each new flow with min=0 and capacity free admits; to keep it bounded
    // we only measure submit cost.
    std::printf("  %-38s %12.0f operations/sec\n", "flow admission (uncontended)", (n / (ms / 1000.0)));
  }
  // allocation decision throughput (pure allocator)
  {
    std::vector<AllocEntry> e(256);
    for (int i = 0; i < 256; ++i) {
      e[i].flow = FlowId(0, static_cast<uint64_t>(i + 1));
      e[i].weight = static_cast<double>(1 + (i % 8));
      e[i].min = 0.0;
      e[i].want = 10.0e6;
      e[i].max = 50.0e6;
    }
    auto t0 = Clock::now();
    const int n = 100000;
    for (int i = 0; i < n; ++i) (void)allocate_weighted(e, 1.0e9);
    double ms = ms_since(t0);
    std::printf("  %-38s %12.0f operations/sec\n", "allocation decision (256 flows)", (n / (ms / 1000.0)));
  }
  // reservation create/release
  {
    Governor g(GovernorConfig{PolicyConfig{}, 2ULL, false}); fill_gov(g);
    auto t0 = Clock::now();
    const int n = 3000;
    for (int i = 0; i < n; ++i) {
      FlowSpec f = flow_spec(static_cast<uint64_t>(i + 1), PathId(0, 1));
      auto r = g.submit_flow(f);
      if (r.reservation) g.release_reservation(r.reservation->id);
    }
    double ms = ms_since(t0);
    std::printf("  %-38s %12.0f operations/sec\n", "reservation create/release", ((2.0 * n) / (ms / 1000.0)));
  }
  // snapshot creation
  {
    Governor g(GovernorConfig{PolicyConfig{}, 3ULL, false}); fill_gov(g);
    for (int i = 0; i < 300; ++i) {
      FlowSpec f = flow_spec(static_cast<uint64_t>(i + 1), PathId(0, 1));
      g.submit_flow(f);
    }
    auto t0 = Clock::now();
    const int n = 1000;
    for (int i = 0; i < n; ++i) (void)g.snapshot();
    double ms = ms_since(t0);
    std::printf("  %-38s %12.0f operations/sec\n", "snapshot creation (2000 flows)", (n / (ms / 1000.0)));
  }
  // persistence: encode + decode
  {
    Governor g(GovernorConfig{PolicyConfig{}, 4ULL, false}); fill_gov(g);
    for (int i = 0; i < 200; ++i) {
      FlowSpec f = flow_spec(static_cast<uint64_t>(i + 1), PathId(0, 1));
      g.submit_flow(f);
    }
    auto bytes = g.save();
    auto t0 = Clock::now();
    const int n = 500;
    for (int i = 0; i < n; ++i) { auto snap = decode_snapshot(bytes.data(), bytes.size()); (void)snap; }
    double ms = ms_since(t0);
    std::printf("  %-38s %12.0f operations/sec\n", "persistence decode (500 flows)", (n / (ms / 1000.0)));
    auto t1 = Clock::now();
    for (int i = 0; i < n; ++i) (void)g.save();
    double ms2 = ms_since(t1);
    std::printf("  %-38s %12.0f operations/sec\n", "persistence encode (500 flows)", (n / (ms2 / 1000.0)));
  }
  // multi-thread scheduling
  for (int nt : {1, 2, 4}) {
    Governor g(GovernorConfig{PolicyConfig{}, 5ULL, false}); fill_gov(g);
    auto t0 = Clock::now();
    const int per_thread = 300;
    std::vector<std::thread> ts;
    for (int t = 0; t < nt; ++t) {
      ts.emplace_back([&, t] {
        for (int i = 0; i < per_thread; ++i) {
          uint64_t n = static_cast<uint64_t>(t * per_thread + i + 1);
          FlowSpec f = flow_spec(n, PathId(0, 1));
          try { g.submit_flow(f); } catch (...) {}
          try { g.advance_ms(1.0); g.tick(); } catch (...) {}
        }
      });
    }
    for (auto& th : ts) th.join();
    double ms = ms_since(t0);
    std::printf("  %-38s %12.0f operations/sec\n", ("{" + std::to_string(nt) + "-thread submit+tick}").c_str(),
                ((double)nt * per_thread / (ms / 1000.0)));
  }
  // CUDA transfer governance
  if (cuda_backend_available()) {
    ResourceSpec res;
    res.id = ResourceId(0, 1);
    res.class_ = ResourceClass::PinnedMemory;
    res.source = "gpu0";
    res.destination = "host";
    res.nominal = Capacity::make(1e9);
    res.generation = fresh_generation<CapacityGeneration>(1);
    auto be = make_cuda_backend(res);
    FlowSpec fs;
    fs.id = FlowId(0, 1);
    fs.source = "gpu0"; fs.destination = "host";
    fs.byte_count = 32ULL << 20;  // 32 MiB flow
    be->submit(fs);
    int64_t total = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < 16; ++i) {
      total += (int64_t)be->step(fs.id, 2ULL << 20, 1.0);
    }
    double ms = ms_since(t0);
    double bytes_sec = (total / (ms / 1000.0));
    std::printf("  %-38s %12.0f bytes/sec\n", "CUDA bounded host<->device transfer", bytes_sec);
  } else {
    std::printf("  (CUDA backend unavailable; skipped)\n");
  }

  std::printf("benchmarks complete\n");
  return 0;
}
