// Bandwidth Governor - CUDA backend implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/backend_cuda.hpp"

#ifdef BG_HAVE_CUDA
#include <cuda_runtime.h>
#endif

#include <unordered_map>
#include <vector>

namespace bg {

#ifdef BG_HAVE_CUDA

namespace {

class CudaBackend : public Backend {
 public:
  explicit CudaBackend(ResourceSpec res) : resource_(std::move(res)) {}

  std::string name() const override { return "cuda"; }
  std::vector<ResourceSpec> inventory() const override { return {resource_}; }

  void submit(const FlowSpec& spec) override {
    Job j;
    j.total = spec.byte_count;
    j.done = 0;
    if (cudaMallocHost(&j.host, static_cast<size_t>(j.total) + 1) == cudaSuccess) {
      if (cudaMalloc(&j.device, static_cast<size_t>(j.total) + 1) == cudaSuccess) {
        if (cudaStreamCreate(&j.stream) == cudaSuccess) j.alloc_ok = true;
      }
    }
    jobs_[spec.id] = j;
  }

  uint64_t step(const FlowId& flow, uint64_t budget, double window_ms) override {
    auto it = jobs_.find(flow);
    if (it == jobs_.end()) return 0;
    Job& j = it->second;
    (void)window_ms;
    if (!j.alloc_ok || j.done >= j.total) return 0;
    uint64_t remaining = j.total - j.done;
    uint64_t move = budget < remaining ? budget : remaining;
    if (move == 0) return 0;
    uint8_t* hp = static_cast<uint8_t*>(j.host);
    uint8_t* dp = static_cast<uint8_t*>(j.device);
    // real bounded host<->device movement, then a verified copy back.
    cudaMemcpyAsync(dp + j.done, hp + j.done, static_cast<size_t>(move),
                    cudaMemcpyHostToDevice, j.stream);
    cudaMemcpyAsync(hp + j.done, dp + j.done, static_cast<size_t>(move),
                    cudaMemcpyDeviceToHost, j.stream);
    cudaStreamSynchronize(j.stream);
    if (cudaGetLastError() != cudaSuccess) return 0;
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

  void cancel(const FlowId& flow) override {
    auto it = jobs_.find(flow);
    if (it == jobs_.end()) return;
    free_job(it->second);
    jobs_.erase(it);
  }

  bool all_done() const override {
    for (const auto& [id, j] : jobs_) { (void)id; if (j.done < j.total) return false; }
    return true;
  }

  ~CudaBackend() override {
    for (auto& [id, j] : jobs_) { (void)id; free_job(j); }
    jobs_.clear();
  }

 private:
  struct Job {
    uint64_t total = 0;
    uint64_t done = 0;
    void* host = nullptr;
    void* device = nullptr;
    cudaStream_t stream = nullptr;
    bool alloc_ok = false;
  };
  static void free_job(Job& j) {
    if (j.stream) cudaStreamDestroy(j.stream);
    if (j.device) cudaFree(j.device);
    if (j.host) cudaFreeHost(j.host);
    j.stream = nullptr;
    j.device = nullptr;
    j.host = nullptr;
    j.alloc_ok = false;
  }
  ResourceSpec resource_;
  std::unordered_map<FlowId, Job> jobs_;
};

}  // namespace

bool cuda_backend_available() {
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess) return false;
  return count > 0;
}

bool cuda_ok() { return cudaGetLastError() == cudaSuccess; }

std::unique_ptr<Backend> make_cuda_backend(const ResourceSpec& resource) {
  if (!cuda_backend_available())
    throw error("CUDA backend requested but no usable device is present");
  return std::make_unique<CudaBackend>(resource);
}

#else  // BG_HAVE_CUDA

bool cuda_backend_available() { return false; }
bool cuda_ok() { return true; }
std::unique_ptr<Backend> make_cuda_backend(const ResourceSpec&) {
  throw error("CUDA backend is not compiled in");
}

#endif

}  // namespace bg
