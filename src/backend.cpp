// Bandwidth Governor - deterministic synthetic backend.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/backend.hpp"

#include <unordered_map>

namespace bg {

// A deterministic synthetic backend used for exhaustive tests and offline
// behaviour validation. It does not fabricate hardware capability: it models
// byte movement at the granted rate, which is the layer the governor actually
// governs. Never used to claim real transfer execution.
class SyntheticBackend : public Backend {
 public:
  explicit SyntheticBackend(std::vector<ResourceSpec> inv)
      : inv_(std::move(inv)) {}

  std::string name() const override { return "synthetic"; }

  std::vector<ResourceSpec> inventory() const override { return inv_; }

  void submit(const FlowSpec& spec) override {
    Job j;
    j.total = spec.byte_count;
    j.done = 0;
    j.name = spec.source + "->" + spec.destination;
    jobs_[spec.id] = std::move(j);
  }

  uint64_t step(const FlowId& flow, uint64_t budget, double window_ms) override {
    auto it = jobs_.find(flow);
    if (it == jobs_.end()) return 0;
    Job& j = it->second;
    (void)window_ms;
    if (j.done >= j.total) return 0;
    uint64_t remaining = j.total - j.done;
    uint64_t move = budget < remaining ? budget : remaining;
    j.done += move;
    return move;
  }

  uint64_t completed(const FlowId& flow) const override {
    auto it = jobs_.find(flow);
    if (it == jobs_.end()) return 0;
    return it->second.done;
  }

  bool is_done(const FlowId& flow) const override {
    auto it = jobs_.find(flow);
    if (it == jobs_.end()) return true;
    return it->second.done >= it->second.total;
  }

  void cancel(const FlowId& flow) override { jobs_.erase(flow); }

  bool all_done() const override {
    for (const auto& [id, j] : jobs_) {
      (void)id;
      if (j.done < j.total) return false;
    }
    return true;
  }

 private:
  struct Job {
    uint64_t total = 0;
    uint64_t done = 0;
    std::string name;
  };
  std::vector<ResourceSpec> inv_;
  std::unordered_map<FlowId, Job> jobs_;
};

std::unique_ptr<Backend> make_synthetic_backend(const std::vector<ResourceSpec>& inv) {
  return std::make_unique<SyntheticBackend>(inv);
}

}  // namespace bg
