#include "Socket.h"

#include <cstring>
#include <mutex>
#include <system_error>

namespace solar::net {

void InitializeSockets() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

int LastSocketError() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

void CloseSocket(SocketHandle socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  ::close(socket);
#endif
}

bool SetNonBlocking(SocketHandle socket) {
#ifdef _WIN32
  u_long on = 1;
  return ioctlsocket(socket, FIONBIO, &on) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string ErrorMessage(int error) {
  if (error == kProtocolError) return "protocol error";
#ifdef _WIN32
  return std::system_category().message(error);
#else
  return std::generic_category().message(error);
#endif
}

#ifndef _WIN32
bool MakeWakePipe(int fds[2]) {
  if (::pipe(fds) != 0) return false;
  for (int i = 0; i < 2; ++i) {
    SetNonBlocking(fds[i]);
    ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
  }
  return true;
}
#endif

int ToSockaddr(const SocketAddress& address, sockaddr_storage& storage) {
  std::memset(&storage, 0, sizeof(storage));
  if (address.family == SocketAddress::Family::IPv6) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&storage);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(address.port);
    std::memcpy(&in6->sin6_addr, address.bytes.data(), 16);
    return static_cast<int>(sizeof(sockaddr_in6));
  }
  auto* in4 = reinterpret_cast<sockaddr_in*>(&storage);
  in4->sin_family = AF_INET;
  in4->sin_port = htons(address.port);
  std::memcpy(&in4->sin_addr, address.bytes.data(), 4);
  return static_cast<int>(sizeof(sockaddr_in));
}

}  // namespace solar::net
