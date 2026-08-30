// Bandworth Governor - resource implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/resource.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace bg {

std::optional<ResourceClass> resource_class_from_string(std::string_view s) noexcept {
  // lowercase the input for case-insensitive matching.
  std::string low;
  low.reserve(s.size());
  for (char c : s) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (low == "pcie") return ResourceClass::Pcie;
  if (low == "nvlink") return ResourceClass::Nvlink;
  if (low == "hostmemory" || low == "host-memory" || low == "host_memory")
    return ResourceClass::HostMemory;
  if (low == "pinnedmemory" || low == "pinned-memory" || low == "pinned_memory")
    return ResourceClass::PinnedMemory;
  if (low == "storageread" || low == "storage-read" || low == "storage_read")
    return ResourceClass::StorageRead;
  if (low == "storagewrite" || low == "storage-write" || low == "storage_write")
    return ResourceClass::StorageWrite;
  if (low == "internodetcp" || low == "inter-node" || low == "inter_node" ||
      low == "internode")
    return ResourceClass::InterNodeTcp;
  if (low == "generic" || low == "generictransport" || low == "generic-transport")
    return ResourceClass::GenericTransport;
  return std::nullopt;
}

std::string_view resource_class_name(ResourceClass c) noexcept {
  switch (c) {
    case ResourceClass::Pcie: return "pcie";
    case ResourceClass::Nvlink: return "nvlink";
    case ResourceClass::HostMemory: return "host_memory";
    case ResourceClass::PinnedMemory: return "pinned_memory";
    case ResourceClass::StorageRead: return "storage_read";
    case ResourceClass::StorageWrite: return "storage_write";
    case ResourceClass::InterNodeTcp: return "inter_node_tcp";
    case ResourceClass::GenericTransport: return "generic_transport";
  }
  return "unknown";
}

Capacity validate_capacity(double raw) {
  if (!Capacity::is_valid_rate(raw)) {
    throw value_error("impossible capacity value: negative, NaN, Inf, or above bound");
  }
  return Capacity::make(raw);
}

double validate_utilization(double u) {
  if (!(u >= 0.0) || !(u <= 1.0) || u != u) {
    throw value_error("utilisation value must be in [0,1] and finite");
  }
  return u;
}

}  // namespace bg
