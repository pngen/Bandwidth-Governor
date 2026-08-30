// Bandwidth Governor - framed wire protocol.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A length-prefixed, magic-fenced, checksummed frame carries strongly typed
// messages between coordinator and workers. Every message is validated for
// framing, version, and checksum before dispatch. Protocol carries all
// authority fields (epoch, boot, attempt, generation) so stale authority is
// rejected deterministically.
#pragma once

#include "bg/core.hpp"
#include "bg/ids.hpp"
#include "bg/flow.hpp"
#include "bg/resource.hpp"
#include "bg/governor.hpp"
#include "bg/persist.hpp"

#include <cstdint>

namespace bg {

inline constexpr uint32_t kWireMagic = 0x46474257u;  // wire frame magic
inline constexpr uint32_t kWireVersion = 1;

enum class WireType : uint32_t {
  Register = 0,        // Worker -> Coord
  RegisterAck = 1,     // Coord -> Worker
  SubmitFlow = 2,      // Client -> Coord
  SubmitAck = 3,       // Coord -> Client
  FlowDispatch = 4,    // Coord -> Worker: run this flow with this grant
  FlowGrant = 5,       // Coord -> Worker: update grant for a running flow
  FlowCancel = 6,      // Coord -> Worker: stop a flow
  Progress = 7,        // Worker -> Coord
  Completion = 8,      // Worker -> Coord
  Ack = 9,             // Coord -> Worker acknowledgement
  QueryResources = 10, // Client -> Coord
  QueryFlows = 11,     // Client -> Coord
  QueryReservations = 12, // Client -> Coord
  Snapshot = 13,       // Client -> Coord: return encoded snapshot
  Save = 14,           // Client -> Coord: save state to path
  Load = 15,           // Client -> Coord: load state from path
  Response = 16,       // Coord -> Client: generic response
  RegisterReject = 17, // Coord -> Worker: registration rejected
  Shutdown = 18,       // Coord -> Worker: clean shutdown
};

const char* wire_type_name(WireType t) noexcept;

// A frame is: magic u32 | version u32 | type u32 | payload_len u32 | payload | crc32 u32.
// The CRC covers the concatenation of type, payload_len and payload.
std::vector<uint8_t> encode_frame(WireType type, const std::vector<uint8_t>& payload);

// Decode a frame. Returns the type and payload. Throws bg::value_error on any
// framing, version, or checksum violation.
struct DecodedFrame {
  WireType type;
  std::vector<uint8_t> payload;
};
DecodedFrame decode_frame(const uint8_t* data, size_t n);

// -------- payload (de)serialisers -------------------------------------------
namespace payload {

struct Register {
  WorkerId worker;
  WorkerBootId boot;
  uint32_t protocol = 1;
  std::string backend;
  std::vector<ResourceSpec> inventory;
};
std::vector<uint8_t> encode_register(const Register& r);
Register decode_register(const uint8_t* p, size_t n);

struct RegisterAck {
  bool ok = false;
  CoordinatorId coordinator;
  CoordinatorEpoch epoch;
  CapacityGeneration generation;
  std::string reason;
};
std::vector<uint8_t> encode_register_ack(const RegisterAck& a);
RegisterAck decode_register_ack(const uint8_t* p, size_t n);

std::vector<uint8_t> encode_flow_spec(const FlowSpec& f);
FlowSpec decode_flow_spec(const uint8_t* p, size_t n);

struct AdmissionWire {
  bool admitted = false;
  FlowSpec flow;
  Decision decision;
  std::optional<Reservation> reservation;
};
std::vector<uint8_t> encode_admission(const AdmissionWire& a);
AdmissionWire decode_admission(const uint8_t* p, size_t n);

struct Dispatch {
  FlowSpec flow;
  AttemptId attempt;
  FlowGeneration fgen;
  WorkerBootId boot;
  Capacity grant;
  ReservationId reservation;
};
std::vector<uint8_t> encode_dispatch(const Dispatch& d);
Dispatch decode_dispatch(const uint8_t* p, size_t n);

struct Grant {
  FlowId flow;
  AttemptId attempt;
  FlowGeneration fgen;
  WorkerBootId boot;
  Capacity grant;
};
std::vector<uint8_t> encode_grant(const Grant& g);
Grant decode_grant(const uint8_t* p, size_t n);

struct Report {
  FlowId flow;
  AttemptId attempt;
  FlowGeneration fgen;
  WorkerBootId boot;
  uint64_t bytes = 0;
  bool completed = false;
};
std::vector<uint8_t> encode_report(const Report& r);
Report decode_report(const uint8_t* p, size_t n);

struct Ack {
  bool ok = false;
  std::string reason;
};
std::vector<uint8_t> encode_ack(const Ack& a);
Ack decode_ack(const uint8_t* p, size_t n);

struct Response {
  bool ok = false;
  std::string message;
  std::vector<uint8_t> body;  // e.g. encoded snapshot blob
};
std::vector<uint8_t> encode_response(const Response& r);
Response decode_response(const uint8_t* p, size_t n);

std::vector<uint8_t> encode_resource_snapshot(const std::vector<ResourceSnapshot>& v);
std::vector<ResourceSnapshot> decode_resource_snapshot(const uint8_t* p, size_t n);
std::vector<uint8_t> encode_flow_snapshot(const std::vector<FlowSnapshot>& v);
std::vector<FlowSnapshot> decode_flow_snapshot(const uint8_t* p, size_t n);
std::vector<uint8_t> encode_reservation_snapshot(const std::vector<Reservation>& v);
std::vector<Reservation> decode_reservation_snapshot(const uint8_t* p, size_t n);

}  // namespace payload
}  // namespace bg
