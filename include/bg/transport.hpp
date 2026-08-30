// Bandwidth Governor - TCP transport.
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A cross-platform TCP transport carries framed governor messages between a
// coordinator OS process and worker OS processes. Blocking network I/O is
// never performed while the governor lock is held: the coordinator serves
// connections on dedicated threads and only briefly locks to apply authority.
#pragma once

#include "bg/wire.hpp"
#include "bg/governor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bg {

// ---- socket abstraction --------------------------------------------------
class TcpSocket {
 public:
  TcpSocket();
  ~TcpSocket();
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;

  bool valid() const noexcept;
  bool connect_to(const std::string& host, uint16_t port);
  bool bind_listen(uint16_t port);
  bool accept_one(TcpSocket& out);
  bool send_all(const uint8_t* data, size_t n);
  bool recv_exact(uint8_t* data, size_t n);
  void close() noexcept;
  bool set_no_delay();
  bool shutdown_send();
  uint16_t local_port() const;

 private:
  void move_from(TcpSocket& other) noexcept;
  void* handle_ = nullptr;  // native SOCKET / fd
};

void net_init();

// ---- frame I/O -----------------------------------------------------------
bool send_frame(TcpSocket& s, WireType type, const std::vector<uint8_t>& payload);
std::optional<DecodedFrame> recv_frame(TcpSocket& s);

// ---- coordinator server --------------------------------------------------
class CoordinatorServer {
 public:
  explicit CoordinatorServer(Governor& gov);
  ~CoordinatorServer();
  // Bind and listen; returns false on failure. Does not accept yet.
  bool listen(uint16_t port);
  void start();
  void stop();
  void join();
  uint16_t port() const;

 private:
  void accept_loop();
  void handle_connection(TcpSocket s);
  void scheduler_loop();
  void unregister_connection(WorkerId w);

  Governor& gov_;
  TcpSocket listen_;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::thread scheduler_thread_;
  std::vector<std::thread> conn_threads_;
  std::mutex conn_mutex_;
  std::unordered_map<WorkerId, TcpSocket*> workers_;  // live worker sockets (owned by connection threads)
  std::mutex workers_mutex_;
  std::unordered_map<FlowId, AttemptId> dispatched_;  // last-dispatched attempt per flow
  uint16_t port_ = 0;
};

}  // namespace bg
