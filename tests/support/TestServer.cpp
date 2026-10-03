#include "TestServer.h"

#include "SocketCompat.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace solar::test {

TestServer::TestServer(Script script, bool ipv6) : script_(std::move(script)) {
  sock::Init();
  listener_ = static_cast<int>(::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0));
  if (listener_ < 0) return;
  int one = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

  sockaddr_storage address{};
  int length;
  if (ipv6) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&address);
    in6->sin6_family = AF_INET6;
    in6->sin6_addr = in6addr_loopback;
    length = static_cast<int>(sizeof(sockaddr_in6));
  } else {
    auto* in4 = reinterpret_cast<sockaddr_in*>(&address);
    in4->sin_family = AF_INET;
    in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    length = static_cast<int>(sizeof(sockaddr_in));
  }

  if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(length)) < 0 || ::listen(listener_, 16) < 0) {
    sock::Close(listener_);
    listener_ = -1;
    return;
  }
  socklen_t nameLength = static_cast<socklen_t>(length);
  ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &nameLength);
  port_ = ntohs(ipv6 ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port : reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  acceptor_ = std::thread([this] { AcceptLoop(); });
}

TestServer::~TestServer() {
  stopping_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  {
    // A script may be waiting on its client; shutting the socket down wakes it.
    std::lock_guard<std::mutex> lock(mutex_);
    for (int client : clients_) sock::ShutdownBoth(static_cast<sock::Handle>(client));
  }
  for (std::thread& worker : workers_) worker.join();
  if (listener_ >= 0) sock::Close(static_cast<sock::Handle>(listener_));
}

void TestServer::AcceptLoop() {
  while (!stopping_) {
    if (!sock::WaitReadable(static_cast<sock::Handle>(listener_), 20)) continue;
    const sock::Handle accepted = sock::Accept(static_cast<sock::Handle>(listener_));
    if (accepted == sock::kInvalid) continue;
    const int client = static_cast<int>(accepted);

    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(client);
    workers_.emplace_back([this, client] {
      script_(client);
      sock::Close(static_cast<sock::Handle>(client));
    });
  }
}

std::string TestServer::ReadHead(int client) {
  std::string head;
  char byte;
  while (head.find("\r\n\r\n") == std::string::npos && sock::Recv(static_cast<sock::Handle>(client), &byte, 1) == 1) head.push_back(byte);
  return head;
}

void TestServer::SendAll(int client, std::string_view data) {
  while (!data.empty()) {
    const long sent = sock::Send(static_cast<sock::Handle>(client), data.data(), data.size());
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
  while (sock::Recv(static_cast<sock::Handle>(client), &byte, 1) > 0) {
  }
}

}  // namespace solar::test
