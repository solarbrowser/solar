#include "TestServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace solar::test {

TestServer::TestServer(Script script, bool ipv6) : script_(std::move(script)) {
  listener_ = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener_ < 0) return;
  int one = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_storage address{};
  socklen_t length;
  if (ipv6) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&address);
    in6->sin6_family = AF_INET6;
    in6->sin6_addr = in6addr_loopback;
    length = sizeof(sockaddr_in6);
  } else {
    auto* in4 = reinterpret_cast<sockaddr_in*>(&address);
    in4->sin_family = AF_INET;
    in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    length = sizeof(sockaddr_in);
  }

  if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), length) < 0 || ::listen(listener_, 16) < 0) {
    ::close(listener_);
    listener_ = -1;
    return;
  }
  ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
  port_ = ntohs(ipv6 ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port : reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  acceptor_ = std::thread([this] { AcceptLoop(); });
}

TestServer::~TestServer() {
  stopping_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  {
    // A script may be waiting on its client; shutting the socket down wakes it.
    std::lock_guard<std::mutex> lock(mutex_);
    for (int client : clients_) ::shutdown(client, SHUT_RDWR);
  }
  for (std::thread& worker : workers_) worker.join();
  if (listener_ >= 0) ::close(listener_);
}

void TestServer::AcceptLoop() {
  while (!stopping_) {
    pollfd waiting{listener_, POLLIN, 0};
    if (::poll(&waiting, 1, 20) <= 0) continue;
    int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) continue;

    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(client);
    workers_.emplace_back([this, client] {
      script_(client);
      ::close(client);
    });
  }
}

std::string TestServer::ReadHead(int client) {
  std::string head;
  char byte;
  while (head.find("\r\n\r\n") == std::string::npos && ::recv(client, &byte, 1, 0) == 1) head.push_back(byte);
  return head;
}

void TestServer::SendAll(int client, std::string_view data) {
  while (!data.empty()) {
    ssize_t sent = ::send(client, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent <= 0) return;
    data.remove_prefix(static_cast<size_t>(sent));
  }
}

void TestServer::SendSlowly(int client, std::string_view data, size_t piece, int delayMs) {
  while (!data.empty()) {
    size_t take = std::min(piece, data.size());
    SendAll(client, data.substr(0, take));
    data.remove_prefix(take);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
  }
}

void TestServer::WaitForClose(int client) {
  char byte;
  while (::recv(client, &byte, 1, 0) > 0) {
  }
}

}  // namespace solar::test
