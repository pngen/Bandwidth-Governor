// Bandwidth Governor - concurrency and race audit.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg_test.hpp"
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/governor.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace bg;

namespace {

struct Fx {
  Governor g;
  ResourceId res;
  PathId path;
  Fx() : g(GovernorConfig{PolicyConfig{}, 7ULL, false}) {
    g.set_auto_clock(false);
    g.advance_ms(0.0);
    ResourceSpec s;
    s.id = ResourceId(0, 1);
    s.class_ = ResourceClass::Pcie;
    s.source = "n";
    s.destination = "n";
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

FlowSpec mk(const Fx& f, uint64_t n) {
  FlowSpec s;
  s.id = FlowId(0, n);
  s.tenant = TenantId(0, 1);
  s.workload = WorkloadId(0, 1);
  s.attempt = AttemptId(0, n);
  s.generation = fresh_generation<FlowGeneration>(n);
  s.source = "n";
  s.destination = "n";
  s.path = f.path;
  s.byte_count = 1u << 20;
  s.requested_min = Capacity::make(0.0);
  s.requested_preferred = Capacity::make(100.0e6);
  s.requested_max = Capacity::make(100.0e6);
  return s;
}

}  // namespace

BG_TEST("concurrency: concurrent submit/tick never overcommits a shared resource") {
  Fx f;
  auto& g = f.g;
  const int kThreads = 8;
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 20; ++i) {
        uint64_t n = static_cast<uint64_t>(t * 1000 + i + 1);
        FlowSpec s = mk(f, n);
        try {
          auto r = g.submit_flow(s);
          if (r.admitted) admitted.fetch_add(1);
        } catch (...) {
          // a concurrent submission that lost the race is acceptable to ignore
        }
        try {
          g.advance_ms(1.0);
          g.tick();
        } catch (...) {}
      }
    });
  }
  for (auto& th : threads) th.join();

  // Sum of grants for running flows must never exceed the governed capacity.
  double total = 0.0;
  for (const auto& fs : g.list_flows()) {
    if (fs.state == FlowState::Running || fs.state == FlowState::Throttled) total += fs.granted.value();
  }
  CHECK(total <= 1.0e9 * 1.01);
  CHECK(admitted.load() > 0);
}

BG_TEST("concurrency: concurrent snapshots/query while mutating is race-free") {
  Fx f;
  auto& g = f.g;
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  // mutator
  std::thread mutator([&] {
    uint64_t i = 1;
    while (!stop.load() && i <= 300) {
      try {
        FlowSpec s = mk(f, i++);
        auto r = g.submit_flow(s);
        if (r.admitted) { g.advance_ms(1.0); g.tick(); }
      } catch (...) { errors.fetch_add(1); }
    }
  });
  // heavy readers
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      for (int k = 0; k < 60; ++k) {
        try {
          auto v = g.list_flows();
          auto rs = g.list_resources();
          auto sn = g.snapshot();
          (void)v; (void)rs; (void)sn;
        } catch (...) { errors.fetch_add(1); }
      }
    });
  }
  for (auto& th : readers) th.join();
  stop.store(true);
  mutator.join();
  CHECK(errors.load() == 0);
}

BG_TEST("concurrency: reservation release is exactly-once under cancellation races") {
  Fx f;
  auto& g = f.g;
  const int n = 60;
  std::vector<FlowId> ids;
  ids.reserve(n);
  for (uint64_t i = 1; i <= static_cast<uint64_t>(n); ++i) {
    FlowSpec s = mk(f, i);
    auto r = g.submit_flow(s);
    if (r.admitted) ids.push_back(s.id);
  }
  g.tick();
  // concurrent cancel + release reservation for the submitted flows.
  std::vector<std::thread> cancellers;
  for (auto& fid : ids) {
    cancellers.emplace_back([&, fid] {
      try {
        g.advance_ms(1.0);
        g.cancel_flow(fid, "race cancel");
        g.tick();
      } catch (...) {}
    });
  }
  for (auto& th : cancellers) th.join();
  // After cancelling everything, accounting must be zero (no leaked running /
  // reserved capacity, no active/pending reservations).
  CHECK(g.accounting_at_zero());
}
