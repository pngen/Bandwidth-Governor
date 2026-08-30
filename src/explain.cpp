// Bandwidth Governor - explainability implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/explain.hpp"

namespace bg {

const char* decision_kind_name(DecisionKind k) noexcept {
  switch (k) {
    case DecisionKind::Admit: return "admit";
    case DecisionKind::Defer: return "defer";
    case DecisionKind::Reject: return "reject";
    case DecisionKind::Throttle: return "throttle";
    case DecisionKind::Preempt: return "preempt";
    case DecisionKind::Grant: return "grant";
    case DecisionKind::Release: return "release";
  }
  return "unknown";
}

}  // namespace bg
