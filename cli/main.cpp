// Bandwidth Governor - command line interface.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/governor.hpp"
#include "bg/transport.hpp"
#include "bg/persist.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace bg;

namespace {
const char* kUsage =
    "Bandwidth Governor CLI\n"
    "  bg resources [--json]\n"
    "  bg paths [--json]\n"
    "  bg flows [--json]\n"
    "  bg reservations [--json]\n"
    "  bg allocations [--json]\n"
    "  bg saturation [--json]\n"
    "  bg explain <flow-id> [--json]\n"
    "  bg submit --bytes N --min R --preferred R --max R --source S --destination D "
    "[--tenant T --workload W --priority P --latency CLASS --deadline SEC --burst B]\n"
    "  bg cancel <flow-id>\n"
    "  bg snapshot [--json]\n"
    "  bg save <path>\n"
    "  bg load <path>\n"
    "  bg demo <scenario>\n"
    "  bg bench <name>\n"
    "  bg serve --port N\n";

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) { case '\\': o += "\\\\"; break; case '"': o += "\\\""; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default: if ((unsigned char)c < 0x20) { char buf[8]; std::snprintf(buf, 8, "\\u%04x", c); o += buf; } else o += c; }
  }
  return o;
}
void build_demo(Governor& g, const std::string& scenario) {
  g.set_auto_clock(false);
  g.advance_ms(0.0);
  ResourceSpec pcie;
  pcie.id = ResourceId(0, 1);
  pcie.class_ = ResourceClass::Pcie;
  pcie.source = "gpu0"; pcie.destination = "host";
  pcie.direction = Directionality::Unidirectional;
  pcie.nominal = Capacity::make(1e9);
  pcie.generation = fresh_generation<CapacityGeneration>(1);
  g.add_resource(pcie);
  if (scenario == "nvlink" || scenario == "shared" || scenario == "priority" ||
      scenario == "fairness" || scenario == "deadline" || scenario == "min" ||
      scenario == "max" || scenario == "burst" || scenario == "throttle" ||
      scenario == "preempt" || scenario == "multilink") {
    ResourceSpec nv;
    nv.id = ResourceId(0, 2); nv.class_ = ResourceClass::Nvlink;
    nv.source = "gpu0"; nv.destination = "gpu1"; nv.nominal = Capacity::make(2e9);
    nv.generation = fresh_generation<CapacityGeneration>(2);
    if (scenario == "nvlink" || scenario == "shared" || scenario == "priority" ||
        scenario == "fairness" || scenario == "deadline" || scenario == "min" ||
        scenario == "max" || scenario == "burst" || scenario == "throttle" ||
        scenario == "preempt") g.add_resource(nv);
  }
  Path p1;
  p1.id = PathId(0, 1); p1.path_generation = fresh_generation<FlowGeneration>(1);
  p1.hops.push_back({pcie.id, pcie.generation});
  g.add_path(p1);
  if (scenario == "nvlink" || scenario == "shared" || scenario == "priority" ||
      scenario == "fairness" || scenario == "deadline" || scenario == "min" ||
      scenario == "max" || scenario == "burst" || scenario == "throttle" ||
      scenario == "preempt") {
    Path p2;
    p2.id = PathId(0, 2); p2.path_generation = fresh_generation<FlowGeneration>(2);
    p2.hops.push_back({ResourceId(0, 2), fresh_generation<CapacityGeneration>(2)});
    g.add_path(p2);
  }
  if (scenario == "multilink") {
    Path pm;
    pm.id = PathId(0, 3); pm.path_generation = fresh_generation<FlowGeneration>(3);
    pm.hops.push_back(p1.hops[0]);
    pm.hops.push_back({ResourceId(0, 2), fresh_generation<CapacityGeneration>(2)});
    g.add_path(pm);
  }
  // submit some competing flows
  auto submit = [&](TenantId t, WorkloadId w, PathId path, double mi, double pr, double mx,
                    size_t n, int prio, LatencyClass lc, double dl) {
    FlowSpec s;
    s.id = FlowId(0, static_cast<uint64_t>(n + 1));
    s.tenant = t; s.workload = w;
    s.attempt = AttemptId(0, static_cast<uint64_t>(n + 1));
    s.generation = fresh_generation<FlowGeneration>(static_cast<uint64_t>(n + 1));
    s.source = "gpu0"; s.destination = "host";
    s.path = path;
    s.byte_count = 1u << 20;
    s.requested_min = Capacity::make(mi);
    s.requested_preferred = Capacity::make(pr);
    s.requested_max = Capacity::make(mx);
    s.priority = prio;
    s.latency_class = lc;
    s.deadline_seconds = dl;
    if (scenario == "burst") s.burst_bytes = 1u << 17;
    g.submit_flow(s);
  };
  if (scenario == "fairness") {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 0, 700e6, 700e6, 1, 0, LatencyClass::ThroughputOriented, 0);
    submit(TenantId(0, 11), WorkloadId(0, 2), p1.id, 0, 700e6, 700e6, 2, 0, LatencyClass::ThroughputOriented, 0);
  } else if (scenario == "priority") {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 0, 800e6, 800e6, 1, 0, LatencyClass::BestEffort, 0);
    submit(TenantId(0, 11), WorkloadId(0, 2), p1.id, 0, 800e6, 800e6, 2, 10, LatencyClass::LatencySensitive, 0);
  } else if (scenario == "deadline") {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 0, 900e6, 900e6, 1, 0, LatencyClass::LatencySensitive, 1.0);
    submit(TenantId(0, 11), WorkloadId(0, 2), p1.id, 0, 900e6, 900e6, 2, 0, LatencyClass::ThroughputOriented, 10.0);
  } else if (scenario == "min") {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 400e6, 800e6, 900e6, 1, 0, LatencyClass::ThroughputOriented, 0);
    submit(TenantId(0, 11), WorkloadId(0, 2), p1.id, 200e6, 600e6, 900e6, 2, 0, LatencyClass::ThroughputOriented, 0);
  } else if (scenario == "max") {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 0, 100e6, 100e6, 1, 0, LatencyClass::ThroughputOriented, 0);
  } else if (scenario == "multilink") {
    submit(TenantId(0, 10), WorkloadId(0, 1), PathId(0, 3), 0, 600e6, 600e6, 1, 0, LatencyClass::ThroughputOriented, 0);
  } else {
    submit(TenantId(0, 10), WorkloadId(0, 1), p1.id, 0, 300e6, 300e6, 1, 0, LatencyClass::BestEffort, 0);
    submit(TenantId(0, 11), WorkloadId(0, 2), p1.id, 0, 300e6, 300e6, 2, 0, LatencyClass::BestEffort, 0);
  }
  g.tick();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "%s", kUsage); return 2; }
  std::string cmd = argv[1];
  bool json = false;
  for (int i = 0; i < argc; ++i) if (std::string(argv[i]) == "--json") json = true;

  if (cmd == "serve") {
    uint16_t port = 0;
    for (int i = 2; i < argc; ++i) { if (std::string(argv[i]) == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoul(argv[i + 1])); }
    if (port == 0) { std::fprintf(stderr, "serve requires --port N\n"); return 2; }
    Governor g(GovernorConfig{});
    // Pre-register resources and paths so client submitters can target them.
    // Worker processes register the same resource ids as their inventory.
    auto add_res = [&](uint64_t rid, ResourceClass cls, double nominal) {
      ResourceSpec s;
      s.id = ResourceId(0, rid);
      s.class_ = cls;
      s.source = "node";
      s.destination = "node";
      s.direction = Directionality::Unidirectional;
      s.nominal = Capacity::make(nominal);
      s.generation = fresh_generation<CapacityGeneration>(rid);
      g.add_resource(s);
    };
    auto add_path = [&](uint64_t pid, uint64_t rid) {
      Path p;
      p.id = PathId(0, pid);
      p.path_generation = fresh_generation<FlowGeneration>(pid);
      p.hops.push_back({ResourceId(0, rid), fresh_generation<CapacityGeneration>(rid)});
      g.add_path(p);
    };
    add_res(1, ResourceClass::Pcie, 1e9);
    add_res(2, ResourceClass::Pcie, 1e9);
    add_path(1, 1);
    add_path(2, 2);
    CoordinatorServer server(g);
    if (!server.listen(port)) { std::fprintf(stderr, "could not bind port %u\n", port); return 1; }
    std::printf("bandwidth-governor coordinator serving on port %u\n", server.port());
    std::fflush(stdout);
    server.start();
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(3600));
    return 0;
  }

  if (cmd == "demo") {
    std::string scenario = argc > 2 ? argv[2] : "fairness";
    Governor g(GovernorConfig{PolicyConfig{}, 4242ULL, false});
    build_demo(g, scenario);
    std::printf("demo scenario: %s\n", scenario.c_str());
    for (const auto& f : g.list_flows()) {
      std::printf("  flow %s tenant %s state %s grant %.0f B/s\n",
                  f.spec.id.to_string().c_str(), f.spec.tenant.to_string().c_str(),
                  flow_state_name(f.state), f.granted.value());
    }
    return 0;
  }

  // Other commands operate on a fresh governor (or one loaded from state).
  Governor g(GovernorConfig{PolicyConfig{}, 777ULL, false});
  g.set_auto_clock(false);
  { for (int i = 0; i < argc; ++i) { if (std::string(argv[i]) == "--state" && i + 1 < argc) { try { g.load_file(argv[i + 1]); } catch (...) {} } } }

  if (cmd == "resources") {
    auto rs = g.list_resources();
    if (json) {
      std::printf("[");
      bool first = true;
      for (const auto& r : rs) {
        if (!first) std::printf(","); first = false;
        std::printf("{\"id\":\"%s\",\"class\":\"%s\",\"source\":\"%s\",\"destination\":\"%s\",\"nominal\":%.0f,\"governed\":%.0f,\"reserved\":%.0f,\"allocated\":%.0f,\"saturated\":%s}",
                    r.id.to_string().c_str(), std::string(resource_class_name(r.class_)).c_str(),
                    json_escape(r.source).c_str(), json_escape(r.destination).c_str(),
                    r.nominal.value(), r.governed.value(), r.reserved.value(), r.allocated.value(),
                    r.saturated ? "true" : "false");
      }
      std::printf("]\n");
    } else {
      for (const auto& r : rs) {
        std::printf("resource %s class=%s %s->%s nominal=%.0f governed=%.0f reserved=%.0f allocated=%.0f gen=%s saturated=%s\n",
                    r.id.to_string().c_str(), std::string(resource_class_name(r.class_)).c_str(),
                    r.source.c_str(), r.destination.c_str(), r.nominal.value(),
                    r.governed.value(), r.reserved.value(), r.allocated.value(),
                    r.capacity_generation.to_string().c_str(), r.saturated ? "yes" : "no");
      }
    }
    return 0;
  }

  if (cmd == "paths") {
    auto ps = g.list_paths();
    for (const auto& p : ps) {
      std::printf("path %s gen=%s hops=%zu\n", p.id.to_string().c_str(),
                  p.path_generation.to_string().c_str(), p.hops.size());
    }
    return 0;
  }

  if (cmd == "flows") {
    auto fs = g.list_flows();
    if (json) {
      std::printf("[");
      bool first = true;
      for (const auto& f : fs) {
        if (!first) std::printf(","); first = false;
        std::printf("{\"id\":\"%s\",\"tenant\":\"%s\",\"state\":\"%s\",\"grant\":%.0f}",
                    f.spec.id.to_string().c_str(), f.spec.tenant.to_string().c_str(),
                    flow_state_name(f.state), f.granted.value());
      }
      std::printf("]\n");
    } else {
      for (const auto& f : fs) {
        std::printf("flow %s tenant %s state=%s grant=%.0f B/s progress=%llu/%llu\n",
                    f.spec.id.to_string().c_str(), f.spec.tenant.to_string().c_str(),
                    flow_state_name(f.state), f.granted.value(),
                    static_cast<unsigned long long>(f.bytes_transferred),
                    static_cast<unsigned long long>(f.spec.byte_count));
      }
    }
    return 0;
  }

  if (cmd == "reservations") {
    for (const auto& r : g.list_reservations()) {
      std::printf("reservation %s flow=%s state=%s epoch=%s\n", r.id.to_string().c_str(),
                  r.flow.to_string().c_str(), reservation_state_name(r.state),
                  r.epoch.to_string().c_str());
    }
    return 0;
  }

  if (cmd == "allocations") {
    for (const auto& [tid, v] : g.tenant_allocations()) {
      std::printf("tenant %s allocated=%.0f\n", tid.to_string().c_str(), v);
    }
    return 0;
  }

  if (cmd == "saturation") {
    for (const auto& r : g.list_resources()) {
      std::printf("resource %s saturated=%s util=%.3f\n", r.id.to_string().c_str(),
                  r.saturated ? "yes" : "no", r.instantaneous_util);
    }
    return 0;
  }

  if (cmd == "explain") {
    if (argc < 3) { std::fprintf(stderr, "explain requires a flow id\n"); return 2; }
    FlowId fid(0, static_cast<uint64_t>(std::stoull(argv[2], nullptr, 16)));
    Decision d = g.explain_flow(fid);
    std::printf("flow %s kind=%s granted=%.0f min=%.0f pref=%.0f max=%.0f starve=%.2fs reason=%s\n",
                d.flow.to_string().c_str(), decision_kind_name(d.kind), d.granted.value(),
                d.minimum_guarantee.value(), d.preferred.value(), d.maximum.value(),
                d.starvation_age, d.kind_reason.c_str());
    for (const auto& f : d.factors)
      std::printf("  factor %s value=%.4g weight=%.4g : %s\n", f.name.c_str(), f.value, f.weight, f.rationale.c_str());
    return 0;
  }

  if (cmd == "submit") {
    auto get = [&](const std::string& n) { std::string v; for (int i = 2; i < argc; ++i) { if (std::string(argv[i]) == n && i + 1 < argc) v = argv[i + 1]; } return v; };
    FlowSpec s;
    s.id = FlowId(0, static_cast<uint64_t>(std::stoull(get("--id").empty() ? "999" : get("--id"), nullptr, 10)));
    s.tenant = TenantId(0, static_cast<uint64_t>(std::stoull(get("--tenant").empty() ? "1" : get("--tenant"), nullptr, 10)));
    s.workload = WorkloadId(0, static_cast<uint64_t>(std::stoull(get("--workload").empty() ? "1" : get("--workload"), nullptr, 10)));
    s.attempt = AttemptId(0, s.id.lo);
    s.generation = fresh_generation<FlowGeneration>(s.id.lo);
    s.source = get("--source").empty() ? "gpu0" : get("--source");
    s.destination = get("--destination").empty() ? "host" : get("--destination");
    s.path = PathId(0, 1);
    s.byte_count = static_cast<uint64_t>(std::stoull(get("--bytes").empty() ? "1048576" : get("--bytes"), nullptr, 10));
    s.requested_min = Capacity::make(std::stod(get("--min").empty() ? "0" : get("--min")));
    s.requested_preferred = Capacity::make(std::stod(get("--preferred").empty() ? "1e8" : get("--preferred")));
    s.requested_max = Capacity::make(std::stod(get("--max").empty() ? "1e9" : get("--max")));
    s.priority = std::stoi(get("--priority").empty() ? "0" : get("--priority"));
    std::string lc = get("--latency");
    if (lc == "latency" || lc == "sensitive") s.latency_class = LatencyClass::LatencySensitive;
    else if (lc == "throughput") s.latency_class = LatencyClass::ThroughputOriented;
    s.deadline_seconds = std::stod(get("--deadline").empty() ? "0" : get("--deadline"));
    s.burst_bytes = static_cast<uint64_t>(std::stoull(get("--burst").empty() ? "0" : get("--burst"), nullptr, 10));
    AdmissionResult r = g.submit_flow(s);
    g.tick();
    std::printf("submit: admitted=%d kind=%s reason=%s\n", (int)r.admitted, decision_kind_name(r.decision.kind), r.decision.kind_reason.c_str());
    return 0;
  }

  if (cmd == "cancel") {
    if (argc < 3) { std::fprintf(stderr, "cancel requires a flow id\n"); return 2; }
    FlowId fid(0, static_cast<uint64_t>(std::stoull(argv[2], nullptr, 16)));
    bool ok = g.cancel_flow(fid, "canceled by CLI");
    std::printf("cancel: %s\n", ok ? "ok" : "no");
    return 0;
  }

  if (cmd == "save") {
    if (argc < 3) return 2;
    g.save_file(argv[2]);
    std::printf("saved to %s\n", argv[2]);
    return 0;
  }
  if (cmd == "load") {
    if (argc < 3) return 2;
    g.load_file(argv[2]);
    std::printf("loaded from %s\n", argv[2]);
    return 0;
  }
  if (cmd == "snapshot") {
    auto s = g.snapshot();
    std::printf("snapshot: flows=%zu resources=%zu reservations=%zu saved_at=%.0fms\n",
                s.flows.size(), s.resources.size(), s.reservations.size(), s.saved_at_ms);
    return 0;
  }
  if (cmd == "bench") {
    std::printf("benchmark scaffolding (use the benchmarks/ suite for full results)\n");
    return 0;
  }

  std::fprintf(stderr, "%s", kUsage);
  return 2;
}
