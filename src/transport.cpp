// Bandwidth Governor - TCP transport implementation.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
#include "bg/transport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>

namespace bg {

#ifdef _WIN32
using rawhandle = SOCKET;
constexpr rawhandle kInvalid = INVALID_SOCKET;
#else
using rawhandle = int;
constexpr rawhandle kInvalid = -1;
#endif

namespace {
rawhandle native(void* h) { return static_cast<rawhandle>(reinterpret_cast<uintptr_t>(h)); }
void* as_void(rawhandle h) { return reinterpret_cast<void*>(static_cast<uintptr_t>(h)); }
std::atomic<bool> g_net_inited{false};
}  // namespace

void net_init() {
#ifdef _WIN32
  if (g_net_inited.exchange(true)) return;
  WSADATA wsadata;
  if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
    throw error("Winsock initialisation failed");
  }
#else
  g_net_inited = true;
#endif
}

TcpSocket::TcpSocket() : handle_(as_void(kInvalid)) {}
TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& other) noexcept { move_from(other); }
TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    move_from(other);
  }
  return *this;
}
void TcpSocket::move_from(TcpSocket& other) noexcept {
  handle_ = other.handle_;
  other.handle_ = as_void(kInvalid);
}
bool TcpSocket::valid() const noexcept { return native(handle_) != kInvalid; }
void TcpSocket::close() noexcept {
  if (native(handle_) != kInvalid) {
#ifdef _WIN32
    ::closesocket(native(handle_));
#else
    ::close(native(handle_));
#endif
    handle_ = as_void(kInvalid);
  }
}

bool TcpSocket::connect_to(const std::string& host, uint16_t port) {
  net_init();
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
    return false;
  bool ok = false;
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    rawhandle s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kInvalid) continue;
    if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
      handle_ = as_void(s);
      ok = true;
      break;
    }
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
  }
  freeaddrinfo(res);
  set_no_delay();
  return ok;
}

bool TcpSocket::bind_listen(uint16_t port) {
  net_init();
  rawhandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalid) return false;
  int yes = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(s, 16) != 0) {
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    return false;
  }
  handle_ = as_void(s);
  return true;
}

bool TcpSocket::accept_one(TcpSocket& out) {
  rawhandle c = ::accept(native(handle_), nullptr, nullptr);
  if (c == kInvalid) return false;
  out.close();
  out.handle_ = as_void(c);
  out.set_no_delay();
  return true;
}

bool TcpSocket::set_no_delay() {
  if (!valid()) return false;
  int one = 1;
  return ::setsockopt(native(handle_), IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
}

bool TcpSocket::shutdown_send() {
#ifdef _WIN32
  return ::shutdown(native(handle_), SD_SEND) == 0;
#else
  return ::shutdown(native(handle_), SHUT_WR) == 0;
#endif
}

bool TcpSocket::shutdown_both() {
#ifdef _WIN32
  return ::shutdown(native(handle_), SD_BOTH) == 0;
#else
  return ::shutdown(native(handle_), SHUT_RDWR) == 0;
#endif
}

uint16_t TcpSocket::local_port() const {
  if (!valid()) return 0;
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  if (::getsockname(native(handle_), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
  return ntohs(a.sin_port);
}

bool TcpSocket::send_all(const uint8_t* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    int sent = ::send(native(handle_), reinterpret_cast<const char*>(data + off),
                      static_cast<int>(n - off), 0);
    if (sent <= 0) return false;
    off += static_cast<size_t>(sent);
  }
  return true;
}

bool TcpSocket::recv_exact(uint8_t* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    int r = ::recv(native(handle_), reinterpret_cast<char*>(data + off),
                   static_cast<int>(n - off), 0);
    if (r == 0) return false;  // peer closed
    if (r < 0) return false;
    off += static_cast<size_t>(r);
  }
  return true;
}

// ---------------------------------------------------------------------------
// frame I/O
// ---------------------------------------------------------------------------
bool send_frame(TcpSocket& s, WireType type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame = encode_frame(type, payload);
  return s.send_all(frame.data(), frame.size());
}

std::optional<DecodedFrame> recv_frame(TcpSocket& s) {
  uint8_t hdr[16];
  if (!s.recv_exact(hdr, 16)) return std::nullopt;
  BinarySource hs(hdr, 16);
  uint32_t magic = hs.u32();
  uint32_t ver = hs.u32();
  uint32_t type = hs.u32();
  uint32_t len = hs.u32();
  if (magic != kWireMagic) throw value_error("bad wire magic on stream");
  if (ver != kWireVersion) throw value_error("bad wire version on stream");
  if (len > (1u << 28)) throw value_error("wire frame too large");
  std::vector<uint8_t> payload(len);
  if (!s.recv_exact(payload.data(), len)) return std::nullopt;
  uint8_t crcb[4];
  if (!s.recv_exact(crcb, 4)) return std::nullopt;
  BinarySource cs(crcb, 4);
  uint32_t crc = cs.u32();
  std::vector<uint8_t> hdr2;
  {
    BinarySink h;
    h.u32(type);
    h.u32(len);
    hdr2 = h.data();
  }
  Crc32 c;
  c.update(hdr2.data(), hdr2.size());
  c.update(payload.data(), payload.size());
  if (crc != c.value()) throw value_error("wire frame checksum mismatch on stream");
  DecodedFrame df;
  df.type = static_cast<WireType>(type);
  df.payload = std::move(payload);
  return df;
}

// ---------------------------------------------------------------------------
// CoordinatorServer
// ---------------------------------------------------------------------------
// A live worker connection owns its accepted socket. The per-connection write
// mutex serialises every write to the peer so the scheduler thread and the
// connection thread cannot interleave their frames on the stream.
struct CoordinatorServer::Conn {
  TcpSocket socket;
  std::mutex write_mutex;
};

CoordinatorServer::CoordinatorServer(Governor& gov) : gov_(gov) {}
CoordinatorServer::~CoordinatorServer() { stop(); }

bool CoordinatorServer::listen(uint16_t port) {
  bool ok = listen_.bind_listen(port);
  if (ok) port_ = listen_.local_port();
  return ok;
}

uint16_t CoordinatorServer::port() const { return port_; }

void CoordinatorServer::start() {
  running_.store(true);
  accept_thread_ = std::thread([this] { accept_loop(); });
  scheduler_thread_ = std::thread([this] { scheduler_loop(); });
}

void CoordinatorServer::stop() {
  if (!running_.exchange(false)) return;
  listen_.close();
  if (accept_thread_.joinable()) accept_thread_.join();
  // signal scheduler loop to exit; it checks running_ each iteration.
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  // Unblock connection threads blocked in recv by shutting down their sockets
  // (this is a shutdown race: a worker connection thread must be able to exit
  // promptly instead of sitting in recv forever).
  {
    std::lock_guard<std::mutex> lk(workers_mutex_);
    for (auto& [wid, conn] : workers_) { (void)wid; conn->socket.shutdown_both(); }
  }
  std::lock_guard<std::mutex> lk(conn_mutex_);
  for (auto& t : conn_threads_) if (t.joinable()) t.join();
  conn_threads_.clear();
}

void CoordinatorServer::join() {
  if (accept_thread_.joinable()) accept_thread_.join();
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  std::lock_guard<std::mutex> lk(conn_mutex_);
  for (auto& t : conn_threads_) if (t.joinable()) t.join();
  conn_threads_.clear();
}

void CoordinatorServer::accept_loop() {
  while (running_) {
    auto conn = std::make_shared<Conn>();
    if (!listen_.accept_one(conn->socket)) {
      if (!running_) break;
      continue;
    }
    std::lock_guard<std::mutex> lk(conn_mutex_);
    conn_threads_.emplace_back([this, conn]() mutable { handle_connection(conn); });
  }
}

void CoordinatorServer::unregister_connection(WorkerId w) {
  std::lock_guard<std::mutex> lk(workers_mutex_);
  workers_.erase(w);
}

void CoordinatorServer::handle_connection(ConnPtr conn) {
  TcpSocket& s = conn->socket;
  // Every write to this peer socket must be serialised with the scheduler
  // thread's writes, or the two frames interleave on the stream.
  auto send_on = [&](WireType type, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> wlk(conn->write_mutex);
    send_frame(s, type, payload);
  };
  while (running_) {
    std::optional<DecodedFrame> opt;
    try {
      opt = recv_frame(s);
    } catch (const std::exception&) {
      break;
    }
    if (!opt) break;
    WireType t = opt->type;
    const std::vector<uint8_t>& body = opt->payload;
    try {
      switch (t) {
        case WireType::Register: {
          auto reg = payload::decode_register(body.data(), body.size());
          gov_.register_worker(WorkerRegistration{reg.worker, reg.boot, reg.inventory,
                                                  reg.protocol, reg.backend});
          {
            std::lock_guard<std::mutex> lk(workers_mutex_);
            workers_.erase(reg.worker);
            workers_[reg.worker] = conn;  // this connection now owns the worker slot
          }
          payload::RegisterAck ack;
          ack.ok = true;
          ack.coordinator = gov_.coordinator();
          ack.epoch = gov_.epoch();
          ack.generation = gov_.capacity_generation();
          send_on(WireType::RegisterAck, payload::encode_register_ack(ack));
          break;  // keep reading this worker's progress/completion on the same connection
        }
        case WireType::SubmitFlow: {
          auto spec = payload::decode_flow_spec(body.data(), body.size());
          AdmissionResult r = gov_.submit_flow(spec);
          payload::AdmissionWire w;
          w.admitted = r.admitted;
          w.flow = spec;
          w.decision = r.decision;
          w.reservation = r.reservation;
          send_on(WireType::SubmitAck, payload::encode_admission(w));
          break;
        }
        case WireType::Progress: {
          auto rep = payload::decode_report(body.data(), body.size());
          bool ok = gov_.report_progress(rep.flow, rep.attempt, rep.fgen, rep.boot, rep.bytes);
          payload::Ack ack; ack.ok = ok;
          send_on(WireType::Ack, payload::encode_ack(ack));
          break;
        }
        case WireType::Completion: {
          auto rep = payload::decode_report(body.data(), body.size());
          bool ok = gov_.report_completion(rep.flow, rep.attempt, rep.fgen, rep.boot, rep.bytes);
          payload::Ack ack; ack.ok = ok;
          send_on(WireType::Ack, payload::encode_ack(ack));
          break;
        }
        case WireType::QueryResources: {
          payload::Response resp;
          resp.ok = true;
          resp.body = payload::encode_resource_snapshot(gov_.list_resources());
          send_on(WireType::Response, payload::encode_response(resp));
          break;
        }
        case WireType::QueryFlows: {
          payload::Response resp;
          resp.ok = true;
          resp.body = payload::encode_flow_snapshot(gov_.list_flows());
          send_on(WireType::Response, payload::encode_response(resp));
          break;
        }
        case WireType::QueryReservations: {
          payload::Response resp;
          resp.ok = true;
          resp.body = payload::encode_reservation_snapshot(gov_.list_reservations());
          send_on(WireType::Response, payload::encode_response(resp));
          break;
        }
        case WireType::Snapshot: {
          auto bytes = gov_.save();
          payload::Response resp;
          resp.ok = true;
          resp.body = bytes;
          send_on(WireType::Response, payload::encode_response(resp));
          break;
        }
        case WireType::Save: case WireType::Load: {
          payload::Response resp;
          resp.ok = true;
          if (t == WireType::Save) gov_.save_file("bandwidth.entity");
          else gov_.load_file("bandwidth.entity");
          send_on(WireType::Response, payload::encode_response(resp));
          break;
        }
        default:
          break;
      }
    } catch (const std::exception& e) {
      payload::Ack ack;
      ack.ok = false;
      ack.reason = e.what();
      send_on(WireType::Ack, payload::encode_ack(ack));
    }
  }
  // On disconnect, unregister this connection from the worker map if it is ours.
  {
    std::lock_guard<std::mutex> lk(workers_mutex_);
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->second == conn) it = workers_.erase(it);
      else ++it;
    }
  }
}

void CoordinatorServer::scheduler_loop() {
  while (running_) {
    try {
      gov_.tick();
    } catch (...) {
      // scheduler must never die
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // dispatch/grants to workers
    auto flows = gov_.list_flows();
    for (const FlowSnapshot& f : flows) {
      if (f.state != FlowState::Running || !f.assigned_worker || !f.reservation) continue;
      auto rsv = gov_.reservation(*f.reservation);
      if (!rsv) continue;
      // Take a reference to the connection (keeps the socket alive), then send
      // OUTSIDE the worker-map lock so a blocking send cannot stall every other
      // connection registration. The per-connection write mutex serialises with
      // the connection thread's own writes so frames never interleave.
      ConnPtr conn;
      {
        std::lock_guard<std::mutex> lk(workers_mutex_);
        auto it = workers_.find(*f.assigned_worker);
        if (it == workers_.end()) continue;
        conn = it->second;
      }
      auto dit = dispatched_.find(f.spec.id);
      bool need_dispatch = (dit == dispatched_.end()) || (dit->second != f.spec.attempt);
      if (need_dispatch) {
        payload::Dispatch d;
        d.flow = f.spec;
        d.attempt = f.spec.attempt;
        d.fgen = f.spec.generation;
        // The dispatch must carry the worker's CURRENT assigned boot, not the
        // (possibly stale/null) reservation's worker_boot. submit_flow mints the
        // reservation before a worker is assigned, so the reservation's
        // worker_boot is null until a worker is bound; a null boot would make the
        // worker's own reports be rejected as stale and the flow never progress.
        d.boot = f.assigned_boot;
        d.grant = f.granted;
        d.reservation = *f.reservation;
        {
          std::lock_guard<std::mutex> wlk(conn->write_mutex);
          send_frame(conn->socket, WireType::FlowDispatch, payload::encode_dispatch(d));
        }
        dispatched_[f.spec.id] = f.spec.attempt;
      } else {
        payload::Grant g;
        g.flow = f.spec.id;
        g.attempt = f.spec.attempt;
        g.fgen = f.spec.generation;
        g.boot = f.assigned_boot;
        g.grant = f.granted;
        {
          std::lock_guard<std::mutex> wlk(conn->write_mutex);
          send_frame(conn->socket, WireType::FlowGrant, payload::encode_grant(g));
        }
      }
    }
    // clean dispatched_ set for flows no longer running
    std::vector<FlowId> running_ids;
    for (const FlowSnapshot& f : flows) {
      if (f.state == FlowState::Running) running_ids.push_back(f.spec.id);
    }
    for (auto it = dispatched_.begin(); it != dispatched_.end();) {
      if (std::find(running_ids.begin(), running_ids.end(), it->first) == running_ids.end()) {
        it = dispatched_.erase(it);  // flow no longer running; allow re-dispatch later
      } else {
        ++it;
      }
    }
  }
}

}  // namespace bg
