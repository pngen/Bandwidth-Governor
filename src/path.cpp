// Bandwidth Governor - path implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/path.hpp"

#include <algorithm>

namespace bg {

std::optional<std::string> validate_path(const Path& p) noexcept {
  if (p.id.is_null()) return "path id is null";
  if (p.path_generation.is_null()) return "path generation is null";
  if (p.hops.empty()) return "path must contain at least one hop";
  std::vector<ResourceId> seen;
  for (const PathHop& hop : p.hops) {
    if (hop.resource.is_null()) return "path hop has null resource id";
    if (hop.generation.is_null()) return "path hop has null resource generation";
    if (std::find(seen.begin(), seen.end(), hop.resource) != seen.end())
      return "path contains a duplicate resource hop";
    seen.push_back(hop.resource);
  }
  return std::nullopt;
}

}  // namespace bg
