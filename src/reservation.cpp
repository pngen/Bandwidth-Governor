// Bandwidth Governor - reservation implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/reservation.hpp"

namespace bg {

const char* reservation_state_name(ReservationState s) noexcept {
  switch (s) {
    case ReservationState::Pending: return "pending";
    case ReservationState::Active: return "active";
    case ReservationState::Released: return "released";
    case ReservationState::Cancelled: return "cancelled";
    case ReservationState::Expired: return "expired";
    case ReservationState::Failed: return "failed";
  }
  return "unknown";
}

// Guarded reservation transition table. Once released/cancelled a reservation is
// terminal: it can never be released yet again (exactly-once release).
std::optional<ReservationState> reservation_transition(ReservationState from,
                                                       ReservationState to) noexcept {
  if (from == to) return to;
  switch (from) {
    case ReservationState::Pending:
      if (to == ReservationState::Active || to == ReservationState::Cancelled ||
          to == ReservationState::Expired || to == ReservationState::Failed)
        return to;
      break;
    case ReservationState::Active:
      if (to == ReservationState::Released || to == ReservationState::Cancelled ||
          to == ReservationState::Expired || to == ReservationState::Failed)
        return to;
      break;
    case ReservationState::Released:
    case ReservationState::Cancelled:
    case ReservationState::Expired:
    case ReservationState::Failed:
      // terminal: no outgoing transition, release is exactly-once.
      break;
  }
  return std::nullopt;
}

}  // namespace bg
