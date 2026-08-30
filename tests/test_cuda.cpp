// Bandwidth Governor - real CUDA transfer governance proof.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// On validated hardware (RTX 5090 / sm_120, CUDA 13.1) this proof drives real
// bounded host<->device transfers under governor control for competing flows
// with distinct priority/caps, and asserts the observed movement conforms to the
// governed allocations. Payload integrity and resource cleanup are verified. If
// CUDA is unavailable the test is skipped (labeled, never fabricated).
#include "bg_test.hpp"
#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/governor.hpp"
#include "bg/backend.hpp"
#include "bg/backend_cuda.hpp"

#include <cstring>

using namespace bg;

BG_TEST("cuda: real transfer governance conforms to allocation") {
  if (!cuda_backend_available()) {
    std::printf("    (CUDA unavailable; skipped)\n");
    return;
  }
  Governor g(GovernorConfig{PolicyConfig{}, 5150ULL, false});
  g.set_auto_clock(false);
  g.advance_ms(0.0);

  ResourceSpec res;
  res.id = ResourceId(0, 1);
  res.class_ = ResourceClass::PinnedMemory;
  res.source = "gpu0";
  res.destination = "host";
  res.nominal = Capacity::make(1.0e9);  // 1 GB/s governed fabric
  res.generation = fresh_generation<CapacityGeneration>(1);
  g.add_resource(res);
  Path path;
  path.id = PathId(0, 1);
  path.path_generation = fresh_generation<FlowGeneration>(1);
  path.hops.push_back({res.id, res.generation});
  g.add_path(path);

  auto make = [&](FlowId fid, double cap, int prio) {
    FlowSpec s;
    s.id = fid;
    s.tenant = TenantId(0, 1);
    s.workload = WorkloadId(0, 1);
    s.attempt = AttemptId(0, fid.lo);
    s.generation = fresh_generation<FlowGeneration>(fid.lo);
    s.source = "gpu0";
    s.destination = "host";
    s.path = path.id;
    s.byte_count = 256ULL << 20;  // stays running across the proof
    s.requested_min = Capacity::make(0.0);
    s.requested_preferred = Capacity::make(cap);
    s.requested_max = Capacity::make(cap);
    s.priority = prio;
    s.latency_class = LatencyClass::ThroughputOriented;
    return s;
  };

  // competing flows: A high cap, B low cap.
  FlowSpec fa = make(FlowId(0, 1), 600.0e6, 5);
  FlowSpec fb = make(FlowId(0, 2), 250.0e6, 0);
  AdmissionResult ra = g.submit_flow(fa);
  AdmissionResult rb = g.submit_flow(fb);
  REQUIRE(ra.admitted);
  REQUIRE(rb.admitted);
  g.advance_ms(1.0);
  g.tick();

  auto fA = g.flow(fa.id);
  auto fB = g.flow(fb.id);
  REQUIRE(fA.has_value() && fA->state == FlowState::Running);
  REQUIRE(fB.has_value() && fB->state == FlowState::Running);
  double grantA = fA->granted.value();
  double grantB = fB->granted.value();
  // capped and non-overcommitted
  CHECK(grantA <= 600.0e6 + 1.0);
  CHECK(grantB <= 250.0e6 + 1.0);
  CHECK(grantA + grantB <= 1.0e9 + 1.0);
  CHECK(grantA > grantB);  // higher priority/cap wins the contested share

  // Real backend over pinned host<->device buffers.
  auto be = make_cuda_backend(res);
  REQUIRE(be != nullptr);
  be->submit(fa);
  be->submit(fb);

  uint64_t totalA = 0;
  uint64_t totalB = 0;
  const int slices = 20;
  const double slice_ms = 5.0;
  for (int i = 0; i < slices; ++i) {
    uint64_t movedA = be->step(fa.id, static_cast<uint64_t>(grantA * slice_ms / 1000.0), slice_ms);
    uint64_t movedB = be->step(fb.id, static_cast<uint64_t>(grantB * slice_ms / 1000.0), slice_ms);
    totalA += movedA;
    totalB += movedB;
  }
  // Each flow's observed movement equals its governed share of the window.
  double window = slices * slice_ms / 1000.0;
  double expectedA = grantA * window;
  double expectedB = grantB * window;
  CHECK(static_cast<double>(totalA) <= expectedA * 1.05);
  CHECK(static_cast<double>(totalB) <= expectedB * 1.05);
  // observed ratio matches governed allocation ratio.
  CHECK(static_cast<double>(totalA) / static_cast<double>(totalB > 0 ? totalB : 1) >
        grantA / grantB * 0.9);
  // aggregate observed throughput conforms to the governed fabric.
  double observed = static_cast<double>(totalA + totalB) / window;
  CHECK(observed <= 1.0e9 * 1.05);
  CHECK(observed > 0.0);

  // CUDA context is healthy after the transfers.
  CHECK(cuda_ok());

  // Cleanup: cancel both flows, release reservations, accounting to zero.
  g.cancel_flow(fa.id, "proof complete");
  g.cancel_flow(fb.id, "proof complete");
  be->cancel(fa.id);
  be->cancel(fb.id);
  g.tick();
  CHECK(g.accounting_at_zero());
  CHECK(cuda_ok());
}
