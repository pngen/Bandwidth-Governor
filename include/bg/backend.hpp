// Bandwidth Governor - backend abstraction.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A backend executes data movement under governor control. The governor grants
// a byte rate; the owning worker drives the backend through bounded pacing
// slices so that the observed rate conforms to the governed allocation. The
// synthetic backend is a deterministic model for exhaustive testing; the CUDA
// backend performs real host<->device transfers on validated hardware. Unreal
// capabilities are never fabricated: only explicitly labelled synthetic models
// are used for absent fabric.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/resource.hpp"
#include "bg/flow.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bg {

class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string name() const = 0;
  // The resource inventory this backend physically backs.
  virtual std::vector<ResourceSpec> inventory() const = 0;
  // Register a job to run. The backend must know the total byte_count to detect
  // completion and to validate payload (where applicable).
  virtual void submit(const FlowSpec& spec) = 0;
  // Drive one bounded pacing slice for a flow, moving at most 'budget' bytes
  // inside 'window_ms'. Returns the number of bytes actually moved.
  virtual uint64_t step(const FlowId& flow, uint64_t budget, double window_ms) = 0;
  // Bytes completed so far for a flow.
  virtual uint64_t completed(const FlowId& flow) const = 0;
  virtual bool is_done(const FlowId& flow) const = 0;
  virtual void cancel(const FlowId& flow) = 0;
  virtual bool all_done() const = 0;  // no active work remains
};

}  // namespace bg
