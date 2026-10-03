#pragma once

#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace solar::net {

struct SocketAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;
};

// Receives what happens on one connection. Every call is made from the thread running the loop.
class ConnectionHandler {
 public:
  virtual ~ConnectionHandler() = default;

  virtual void OnConnected() = 0;
  // `data` is the buffer the kernel filled, not a copy: it is valid only until this returns.
  virtual void OnData(std::span<const uint8_t> data) = 0;
  // The connection is over: `error` is an errno value, or 0 when the peer closed it cleanly.
  // The Connection is destroyed right after this returns.
  virtual void OnClosed(int error) = 0;
};

// An opaque handle, valid until the handler's OnClosed returns.
class Connection {
 public:
  // Queues `data` to be written as it is, without copying it again.
  void Send(std::string data);
  // Idempotent. OnClosed follows once everything in flight has finished.
  void Close();

 protected:
  Connection() = default;
  ~Connection() = default;
};

// A single-threaded event loop over io_uring. Bytes are received into a ring of buffers the
// kernel picks from, so a connection needs no buffer of its own and nothing is copied on the
// way to the handler.
class Loop {
 public:
  // Null where io_uring cannot be started.
  static std::unique_ptr<Loop> Create();
  ~Loop();

  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;

  // Starts connecting. `handler` must outlive the connection; its OnClosed is the last call.
  Connection* Connect(const SocketAddress& address, ConnectionHandler* handler);

  using Task = std::function<void()>;
  using TimerId = uint64_t;
  TimerId PostDelayed(std::chrono::milliseconds delay, Task task);
  // False when the timer already ran or never existed.
  bool CancelTimer(TimerId id);

  // Runs until no connection is open and no timer is pending, or Stop() is called.
  void Run();
  void Stop();

  struct Impl;

 private:
  Loop();
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
