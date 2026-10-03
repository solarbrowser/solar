#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace solar::net {

// An IP address and port, in no platform's socket structure: each backend converts it.
struct SocketAddress {
  enum class Family : uint8_t { IPv4, IPv6 };

  Family family = Family::IPv4;
  std::array<uint8_t, 16> bytes{};  // network order; the first 4 for IPv4
  uint16_t port = 0;                // host order
};

// One end of a byte stream, as an exchange sees it: bytes go out with Send and the stream ends
// with Close. A TCP connection is one; TLS over a connection is another.
class Transport {
 public:
  // Queues `data` to be written as it is, without copying it again.
  virtual void Send(std::string data) = 0;
  // Idempotent. The end is reported to the handler's OnClosed once everything in flight is done.
  virtual void Close() = 0;

 protected:
  ~Transport() = default;
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
class Connection : public Transport {
 public:
  void Send(std::string data) override;
  void Close() override;
  // An idle connection does not keep Loop::Run going: a pool's idle connections wait for the next
  // request, which may never come. A connection starts out busy.
  void SetIdle(bool idle);

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
  // A background timer does not keep Loop::Run going either, as for housekeeping that only
  // matters while something else is.
  TimerId PostDelayed(std::chrono::milliseconds delay, Task task, bool background = false);
  // False when the timer already ran or never existed.
  bool CancelTimer(TimerId id);

  // Runs until no busy connection is open and no foreground timer is pending, or Stop() is called.
  void Run();
  void Stop();

  struct Impl;

 private:
  Loop();
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
