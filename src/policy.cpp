// Bandwidth Governor - deterministic weighted water-fill allocator.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bg {

namespace {
constexpr double kEps = 1e-9;

// A running contender for the water-fill.
struct Slot {
  std::size_t index = 0;
  double weight = 1.0;
  double min = 0.0;
  double want = 0.0;
  double max = 0.0;
  double granted = 0.0;
  bool active = true;
};
}  // namespace

AllocResult allocate_weighted(const std::vector<AllocEntry>& entries, double capacity) {
  AllocResult out;
  out.granted.resize(entries.size(), 0.0);

  if (entries.empty() || capacity <= 0.0) {
    out.total_requested = 0.0;
    out.total_granted = 0.0;
    out.unserved = 0.0;
    out.saturated = false;
    return out;
  }

  std::vector<Slot> slots;
  slots.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const AllocEntry& e = entries[i];
    double w = e.weight;
    if (!(w > 0.0) || !(w < kMaxRate)) w = 1.0;  // sanitise weight
    Slot s;
    s.index = i;
    s.weight = w;
    s.min = std::clamp(e.min, 0.0, e.max);
    s.want = std::clamp(e.want, s.min, e.max);
    s.max = e.max;
    slots.push_back(s);
    out.total_requested += s.want;
  }

  // Phase 1: honour minimum guarantees.
  double min_sum = 0.0;
  for (Slot& s : slots) {
    s.granted = s.min;
    min_sum += s.min;
  }

  // If minima exceed capacity, scale down minima by weight (deterministic).
  if (min_sum > capacity + kEps) {
    double scale = capacity / min_sum;
    std::vector<double> val(entries.size(), 0.0);
    double assigned = 0.0;
    for (Slot& s : slots) {
      double g = s.min * scale;
      val[s.index] = g;
      assigned += g;
    }
    // Distribute rounding leftovers proportionally by weight (bounded by min).
    double leftover = capacity - assigned;
    if (leftover > kEps) {
      double wsum = 0.0;
      for (Slot& s : slots) wsum += s.weight;
      if (wsum > 0.0) {
        for (Slot& s : slots) {
          val[s.index] += leftover * (s.weight / wsum);
        }
      }
    }
    out.saturated = (out.total_requested > capacity + kEps);
    out.total_granted = 0.0;
    for (double raw : val) out.total_granted += raw;
    out.unserved = std::max(0.0, out.total_requested - out.total_granted);
    out.deadline_pressure = 0.0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      out.granted[i] = std::clamp(val[i], 0.0, entries[i].max);
    }
    return out;
  }

  // Phase 2: water-fill the remainder.
  double remaining = capacity - min_sum;
  std::vector<Slot*> active;
  for (Slot& s : slots) {
    if (s.want > s.granted + kEps) active.push_back(&s);
  }

  // Deterministic tie-break: order active slots by flow id (ascending) then index.
  std::stable_sort(active.begin(), active.end(), [&](Slot* a, Slot* b) {
    const AllocEntry& ea = entries[a->index];
    const AllocEntry& eb = entries[b->index];
    if (ea.flow != eb.flow) return ea.flow < eb.flow;
    return a->index < b->index;
  });

  while (remaining > kEps && !active.empty()) {
    double wsum = 0.0;
    for (Slot* s : active) wsum += s->weight;
    if (wsum <= 0.0) break;

    // Increment per weight-unit if we distribute all remaining now.
    double inc_units = remaining / wsum;

    // Find the active slot that reaches its target earliest.
    double min_room = std::numeric_limits<double>::infinity();
    for (Slot* s : active) {
      double room = (s->want - s->granted) / s->weight;
      if (room < min_room) min_room = room;
    }
    double step = inc_units;
    if (min_room < step) step = min_room;

    for (Slot* s : active) s->granted += step * s->weight;
    double consumed = step * wsum;
    remaining -= consumed;
    if (remaining < 0.0) remaining = 0.0;

    // Remove slots that reached their target.
    active.erase(std::remove_if(active.begin(), active.end(),
                                [](Slot* s) { return s->granted >= s->want - kEps; }),
                 active.end());
  }

  // Phase 3: never exceed max; clamp. If clamping removed capacity, no reuse.
  out.total_granted = 0.0;
  for (Slot& s : slots) {
    double g = std::clamp(s.granted, 0.0, s.max);
    out.granted[s.index] = g;
    s.granted = g;
    out.total_granted += g;
  }
  out.unserved = std::max(0.0, out.total_requested - out.total_granted);
  out.saturated = (out.total_requested > capacity + kEps);
  return out;
}

}  // namespace bg
