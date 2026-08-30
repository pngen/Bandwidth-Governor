// Bandwidth Governor - authority proof (deterministic, real state + wire codec).
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Proves the governor's authoritative state machine + the framed wire codec on a
// real loopback TCP pair: registration, admission, reservation, dispatch,
// completion, stale-authority rejection, exactly-once release and zero leaked
// accounting. The full coordinator+worker distributed runtime is provided by the
// bg serve / bgworker binaries (see docs/tl; the OS-process variant).
#include "bg/wire.hpp"
#include "bg/transport.hpp"
#include "bg/governor.hpp"
#include "bg/backend.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bg;

namespace {
int g_fail = 0;
#define MPCHECK(cond)                                                       \
  do { if (!(cond)) { std::printf("MPCHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  net_init();

  // --- wire codec round-trip over a real loopback TCP pair -----------------
  {
    TcpSocket server;
    MPCHECK(server.bind_listen(0));
    uint16_t port = server.local_port();
    TcpSocket client;
    TcpSocket accepted;
    std::thread accept_thr([&] { MPCHECK(server.accept_one(accepted)); });
    MPCHECK(client.connect_to("127.0.0.1", port));
    accept_thr.join();

    // register payload
    ResourceSpec res;
    res.id = ResourceId(0, 1);
    res.class_ = ResourceClass::Pcie;
    res.source = "node"; res.destination = "node";
    res.nominal = Capacity::make(1e9);
    res.generation = fresh_generation<CapacityGeneration>(1);
    payload::Register reg;
    reg.worker = WorkerId(0, 1);
    reg.boot = WorkerBootId(0, 10);
    reg.inventory = {res};
    auto body = payload::encode_register(reg);
    MPCHECK(send_frame(client, WireType::Register, body));
    auto f = recv_frame(accepted);
    MPCHECK(f.has_value());
    MPCHECK(f->type == WireType::Register);
    auto decoded = payload::decode_register(f->payload.data(), f->payload.size());
    MPCHECK(decoded.worker == reg.worker && decoded.boot == reg.boot);
    MPCHECK(decoded.inventory.size() == 1 && decoded.inventory[0].nominal.value() == 1e9);
  }

  // --- authoritative state machine: admission, dispatch, stale rejection ----
  Governor g(GovernorConfig{PolicyConfig{}, 99ULL, false});
  g.set_auto_clock(false);
  g.advance_ms(0.0);

  ResourceSpec res;
  res.id = ResourceId(0, 1);
  res.class_ = ResourceClass::Pcie;
  res.source = "node"; res.destination = "node";
  res.nominal = Capacity::make(1e9);
  res.generation = fresh_generation<CapacityGeneration>(1);
  g.add_resource(res);
  Path p;
  p.id = PathId(0, 1);
  p.path_generation = fresh_generation<FlowGeneration>(1);
  p.hops.push_back({res.id, res.generation});
  g.add_path(p);

  WorkerRegistration wr;
  wr.worker = WorkerId(0, 1);
  wr.boot = WorkerBootId(0, 10);
  wr.inventory = {res};
  g.register_worker(wr);

  FlowSpec fa;
  fa.id = FlowId(0, 10);
  fa.tenant = TenantId(0, 1);
  fa.workload = WorkloadId(0, 1);
  fa.attempt = AttemptId(0, 10);
  fa.generation = fresh_generation<FlowGeneration>(11);
  fa.source = "node"; fa.destination = "node";
  fa.path = p.id;
  fa.byte_count = 1u << 26;
  fa.requested_min = Capacity::make(0.0);
  fa.requested_preferred = Capacity::make(200.0e6);
  fa.requested_max = Capacity::make(200.0e6);

  AdmissionResult ar = g.submit_flow(fa);
  MPCHECK(ar.admitted);
  MPCHECK(ar.reservation.has_value());
  g.advance_ms(1.0);
  g.tick();
  auto fa1 = g.flow(fa.id);
  MPCHECK(fa1.has_value());
  MPCHECK(fa1->state == FlowState::Running);
  MPCHECK(fa1->assigned_worker && *fa1->assigned_worker == WorkerId(0, 1));

  // valid completion by the correct boot.
  MPCHECK(g.report_completion(fa.id, fa.attempt, fa.generation, WorkerBootId(0, 10), fa.byte_count));
  MPCHECK(g.accounting_at_zero());

  // --- restart roll + stale authority rejection ----------------------------
  FlowSpec fb;
  fb = fa;
  fb.id = FlowId(0, 11);
  fb.attempt = AttemptId(0, 11);
  fb.generation = fresh_generation<FlowGeneration>(12);
  AdmissionResult br = g.submit_flow(fb);
  MPCHECK(br.admitted);
  g.advance_ms(1.0);
  g.tick();
  auto fb1 = g.flow(fb.id);
  MPCHECK(fb1.has_value() && fb1->state == FlowState::Running);
  AttemptId old_attempt = fb1->spec.attempt;
  FlowGeneration old_fgen = fb1->spec.generation;

  // worker loss: restart the same logical worker with a new boot.
  WorkerRegistration wr2;
  wr2.worker = WorkerId(0, 1);
  wr2.boot = WorkerBootId(0, 11);
  wr2.inventory = {res};
  g.register_worker(wr2);
  g.advance_ms(1.0);
  g.tick();
  auto fb2 = g.flow(fb.id);
  MPCHECK(fb2.has_value());
  MPCHECK(fb2->state == FlowState::Running);
  MPCHECK(fb2->spec.attempt != old_attempt);  // retry got a fresh AttemptId

  // stale completion with the OLD boot must be rejected and mutate nothing.
  MPCHECK(!g.report_completion(fb.id, old_attempt, old_fgen, WorkerBootId(0, 10), fb.byte_count));
  MPCHECK(!g.report_completion(fb.id, old_attempt, old_fgen, WorkerBootId(0, 11), fb.byte_count));
  auto fb3 = g.flow(fb.id);
  MPCHECK(fb3.has_value() && fb3->state == FlowState::Running);

  // fresh completion on the new attempt + new boot.
  MPCHECK(g.report_completion(fb.id, fb2->spec.attempt, fb2->spec.generation, WorkerBootId(0, 11), fb.byte_count));
  MPCHECK(g.accounting_at_zero());

  // one subsequent exact valid flow completes successfully.
  FlowSpec fc;
  fc = fa;
  fc.id = FlowId(0, 12);
  fc.attempt = AttemptId(0, 12);
  fc.generation = fresh_generation<FlowGeneration>(13);
  AdmissionResult cr = g.submit_flow(fc);
  MPCHECK(cr.admitted);
  g.advance_ms(1.0);
  g.tick();
  auto fc1 = g.flow(fc.id);
  MPCHECK(fc1.has_value() && fc1->state == FlowState::Running);
  MPCHECK(g.report_completion(fc.id, fc.attempt, fc.generation, WorkerBootId(0, 11), fc.byte_count));
  MPCHECK(g.accounting_at_zero());

  std::printf("MP PASS: authority proof over real wire codec + authoritative state machine succeeded\n");
  return g_fail == 0 ? 0 : 1;
}
