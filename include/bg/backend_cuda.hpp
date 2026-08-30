// Bandwidth Governor - CUDA backend.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A real-transfer backend that performs bounded host<->device copies under
// governor control on validated hardware (RTX 5090 / sm_120). It never
// fabricates absent fabric: only physically available device transfers are
// exercised. Other hardware is represented through explicitly labelled
// synthetic models.
#pragma once

#include "bg/backend.hpp"

namespace bg {

// Returns true when a CUDA device is present and usable (validated at runtime).
bool cuda_backend_available();

// Build a CUDA backend backing a host<->device transfer resource. Throws
// bg::error if CUDA is unavailable.
std::unique_ptr<Backend> make_cuda_backend(const ResourceSpec& resource);

}  // namespace bg
