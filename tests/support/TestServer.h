#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace solar::test {

// A server on the loopback interface whose behaviour is a function: it is called with the socket
// of each connection that arrives, on a thread of its own, and does whatever the test needs.
class TestServer {
 public:
  using Script = std::function<void(int client)>;

  explicit TestServer(Script script, bool ipv6 = false);
  ~TestServer();

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  bool ok() const { return listener_ >= 0; }
  uint16_t port() const { return port_; }

  // Reads until the blank line that ends a request head.
  static std::string ReadHead(int client);
  // Reads `length` bytes, or fewer if the peer closes first: a request body after its head.
  static std::string ReadBytes(int client, size_t length);
  static void SendAll(int client, std::string_view data);
  static void SendSlowly(int client, std::string_view data, size_t piece, int delayMs);
  // Returns once the peer has closed its end.
  static void WaitForClose(int client);

 private:
  void AcceptLoop();

  Script script_;
  int listener_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;

  std::mutex mutex_;
  std::vector<std::thread> workers_;
  std::vector<int> clients_;
};

}  // namespace solar::test
