#pragma once

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Socket.h"
#include "solar/net/Loop.h"

namespace solar::net::internal {

struct ConnectionState final : Connection {
  Loop::Impl* loop = nullptr;
  ConnectionHandler* handler = nullptr;
  SocketAddress address;
  uint64_t id = 0;
  SocketHandle socket = kInvalidSocket;

  bool connected = false;
  bool closing = false;
  bool idle = false;
  bool finalizeQueued = false;
  int error = 0;

  std::deque<std::string> sendQueue;
  size_t sendOffset = 0;

  // What the backend keeps for this connection. Allocated by it, released by its Release.
  void* backendData = nullptr;
};

// What a platform provides to the loop. The core owns the connections, the timers and the
// order things end in; a backend does the I/O and reports what happened through the Loop::Impl
// functions below.
class Backend {
 public:
  virtual ~Backend() = default;

  virtual const char* Name() const = 0;
  // Creates c.socket and starts connecting it. Reports Connected, now or later.
  virtual void Connect(ConnectionState& c) = 0;
  // The connection is up: start receiving. Reports Received, PeerClosed or ReceiveFailed.
  virtual void StartReceive(ConnectionState& c) = 0;
  // Data is waiting in c.sendQueue. Writes it as the socket allows and reports Sent or SendFailed.
  virtual void WantSend(ConnectionState& c) = 0;
  // The connection is closing: stop its I/O. Calls Quiescent once nothing of it is in flight,
  // which may be inside this call.
  virtual void Close(ConnectionState& c) = 0;
  // The core is about to delete c: close its socket and free its backendData.
  virtual void Release(ConnectionState& c) = 0;
  // Waits up to `timeoutMs` (-1: until something happens) and reports what it finds. False on a
  // failure the loop cannot go on after.
  virtual bool Wait(int timeoutMs) = 0;
};

}  // namespace solar::net::internal

struct solar::net::Loop::Impl {
  using ConnectionState = internal::ConnectionState;
  using Clock = std::chrono::steady_clock;

  std::unique_ptr<internal::Backend> backend;
  uint64_t nextConnectionId = 1;
  std::unordered_map<uint64_t, ConnectionState*> connections;
  size_t busyConnections = 0;  // open and not idle
  std::vector<ConnectionState*> finalizable;
  bool stopped = false;

  struct Timer {
    Clock::time_point when;
    Task task;
    bool background;
  };
  TimerId nextTimer = 1;
  std::set<std::pair<Clock::time_point, TimerId>> timerOrder;
  std::unordered_map<TimerId, Timer> timers;
  size_t foregroundTimers = 0;

  bool Pending() const { return busyConnections != 0 || foregroundTimers != 0; }
  void RunTimers();
  void DrainFinalizable();
  int TimeoutMs() const;
  void CloseConnection(ConnectionState& c);
  void QueueFinalize(ConnectionState& c);

  // ---- What a backend reports ----
  void Connected(ConnectionState& c, int error);
  void Received(ConnectionState& c, std::span<const uint8_t> data);
  void PeerClosed(ConnectionState& c);
  void ReceiveFailed(ConnectionState& c, int error);
  // `bytes` of the front of the send queue were written.
  void Sent(ConnectionState& c, size_t bytes);
  void SendFailed(ConnectionState& c, int error);
  void Quiescent(ConnectionState& c);
  // What of the front of the send queue is still to be written; empty if nothing is queued.
  std::string_view PendingSend(const ConnectionState& c) const;
};

namespace solar::net::internal {

// Each is defined by the file for that platform, and returns null where it does not apply or
// cannot start.
std::unique_ptr<Backend> MakeUringBackend(Loop::Impl& core);
std::unique_ptr<Backend> MakeReadinessBackend(Loop::Impl& core);
std::unique_ptr<Backend> MakeIocpBackend(Loop::Impl& core);

}  // namespace solar::net::internal
