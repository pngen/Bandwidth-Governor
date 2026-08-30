// Bandwidth Governor - worker runtime.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A worker process registers a backend with a remote coordinator, dispatches
// flows it is assigned, paces their movement to the granted rate, and reports
// progress/completion. It never fabricates hardware capability.
#include "bg/backend.hpp"
#include "bg/backend_cuda.hpp"
#include "bg/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace bg;

namespace {

struct Job {
  FlowSpec spec;
  AttemptId attempt;
  FlowGeneration fgen;
  WorkerBootId boot;
  ReservationId reservation;
  Capacity grant;
};

std::string arg_value(int argc, char** argv, const std::string& name) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == name && i + 1 < argc) return argv[i + 1];
  return {};
}

uint64_t arg_u64(int argc, char** argv, const std::string& name, uint64_t def) {
  std::string v = arg_value(argc, argv, name);
  if (v.empty()) return def;
  return static_cast<uint64_t>(std::stoull(v, nullptr, 10));
}

template <typename T>
T arg_id(int argc, char** argv, const std::string& name) {
  std::string v = arg_value(argc, argv, name);
  if (v.empty()) return T{};
  return T(0, static_cast<uint64_t>(std::stoull(v, nullptr, 10)));
}

}  // namespace

int main(int argc, char** argv) {
  std::string coord = arg_value(argc, argv, "--coordinator");
  if (coord.empty()) {
    std::fprintf(stderr, "usage: bgworker --coordinator host:port --worker-id N --boot N --backend synthetic|cuda --nominal N\n");
    return 2;
  }
  WorkerId wid = arg_id<WorkerId>(argc, argv, "--worker-id");
  WorkerBootId boot = arg_id<WorkerBootId>(argc, argv, "--boot");
  std::string backend = arg_value(argc, argv, "--backend");
  if (backend.empty()) backend = "synthetic";
  uint64_t nominal = arg_u64(argc, argv, "--nominal", 1000000000ULL);

  std::string host = coord;
  uint16_t port = 0;
  std::size_t colon = coord.find(':');
  if (colon != std::string::npos) {
    host = coord.substr(0, colon);
    port = static_cast<uint16_t>(std::stoul(coord.substr(colon + 1)));
  }
  if (port == 0) { std::fprintf(stderr, "bad coordinator port\n"); return 2; }

  net_init();
  TcpSocket sock;
  if (!sock.connect_to(host, port)) { std::fprintf(stderr, "could not connect to %s:%u\n", host.c_str(), port); return 1; }

  ResourceSpec res;
  res.id = ResourceId(0, wid.lo);
  res.class_ = ResourceClass::Pcie;
  res.source = "worker";
  res.destination = "host";
  res.direction = Directionality::Unidirectional;
  res.nominal = Capacity::make(static_cast<double>(nominal));
  res.generation = fresh_generation<CapacityGeneration>(wid.lo);

  payload::Register reg;
  reg.worker = wid;
  reg.boot = boot;
  reg.protocol = 1;
  reg.backend = backend;
  reg.inventory = {res};

  if (!send_frame(sock, WireType::Register, payload::encode_register(reg))) {
    std::fprintf(stderr, "registration send failed\n");
    return 1;
  }
  auto ackf = recv_frame(sock);
  if (!ackf || ackf->type != WireType::RegisterAck) {
    std::fprintf(stderr, "no register ack\n");
    return 1;
  }
  auto ack = payload::decode_register_ack(ackf->payload.data(), ackf->payload.size());
  if (!ack.ok) { std::fprintf(stderr, "register rejected: %s\n", ack.reason.c_str()); return 1; }

  std::unique_ptr<Backend> be;
  if (backend == "cuda") {
    if (!cuda_backend_available()) { std::fprintf(stderr, "cuda requested but unavailable\n"); return 1; }
    be = make_cuda_backend(res);
  } else {
    be = make_synthetic_backend({res});
  }

  std::mutex mu;
  std::deque<DecodedFrame> inbox;
  std::atomic<bool> done{false};
  std::unordered_map<FlowId, Job> jobs;
  std::unordered_map<FlowId, double> grants;
  std::unordered_map<FlowId, uint64_t> accum;
  std::mutex send_mu;

  std::thread recv([&] {
    auto* sock_ptr = &sock;
    while (!done.load()) {
      std::optional<DecodedFrame> f;
      try { f = recv_frame(*sock_ptr); }
      catch (const std::exception&) { break; }
      if (!f) break;
      std::lock_guard<std::mutex> lk(mu);
      inbox.push_back(std::move(*f));
    }
  });

  auto last_pace = std::chrono::steady_clock::now();
  while (!done.load()) {
    std::vector<DecodedFrame> msgs;
    {
      std::lock_guard<std::mutex> lk(mu);
      while (!inbox.empty()) { msgs.push_back(std::move(inbox.front())); inbox.pop_front(); }
    }
    for (auto& m : msgs) {
      if (m.type == WireType::FlowDispatch) {
        auto d = payload::decode_dispatch(m.payload.data(), m.payload.size());
        Job j;
        j.spec = d.flow;
        j.attempt = d.attempt;
        j.fgen = d.fgen;
        j.boot = d.boot;
        j.reservation = d.reservation;
        j.grant = d.grant;
        be->submit(d.flow);
        {
          std::lock_guard<std::mutex> lk(mu);
          jobs[d.flow.id] = j;
          grants[d.flow.id] = d.grant.value();
          accum[d.flow.id] = 0;
        }
      } else if (m.type == WireType::FlowGrant) {
        auto g = payload::decode_grant(m.payload.data(), m.payload.size());
        std::lock_guard<std::mutex> lk(mu);
        grants[g.flow] = g.grant.value();
      } else if (m.type == WireType::FlowCancel || m.type == WireType::Shutdown) {
        done.store(true);
      }
    }

    auto now = std::chrono::steady_clock::now();
    double delta_ms = std::chrono::duration<double, std::milli>(now - last_pace).count();
    if (delta_ms >= 1.0) {
      last_pace = now;
      std::vector<FlowId> to_report;
      std::vector<FlowId> to_complete;
      std::vector<FlowId> to_erase;
      {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [fid, j] : jobs) {
          double rate = grants[fid];
          uint64_t budget = static_cast<uint64_t>(rate * delta_ms / 1000.0);
          uint64_t moved = be->step(fid, budget, delta_ms);
          accum[fid] += moved;
          if (be->is_done(fid)) to_complete.push_back(fid);
          else if (moved > 0) to_report.push_back(fid);
        }
      }
      for (const auto& fid : to_report) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = jobs.find(fid);
        if (it == jobs.end()) continue;
        payload::Report rep;
        rep.flow = fid;
        rep.attempt = it->second.attempt;
        rep.fgen = it->second.fgen;
        rep.boot = it->second.boot;
        rep.bytes = accum[fid];
        rep.completed = false;
        std::lock_guard<std::mutex> sl(send_mu);
        send_frame(sock, WireType::Progress, payload::encode_report(rep));
      }
      for (const auto& fid : to_complete) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = jobs.find(fid);
        if (it == jobs.end()) continue;
        payload::Report rep;
        rep.flow = fid;
        rep.attempt = it->second.attempt;
        rep.fgen = it->second.fgen;
        rep.boot = it->second.boot;
        rep.bytes = accum[fid];
        rep.completed = true;
        std::lock_guard<std::mutex> sl(send_mu);
        send_frame(sock, WireType::Completion, payload::encode_report(rep));
        to_erase.push_back(fid);
      }
      for (const auto& fid : to_erase) {
        {
          std::lock_guard<std::mutex> lk(mu);
          jobs.erase(fid);
          grants.erase(fid);
          accum.erase(fid);
        }
        be->cancel(fid);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  recv.detach();
  {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [id, j] : jobs) { (void)id; (void)j; be->cancel(id); }
  }
  return 0;
}
