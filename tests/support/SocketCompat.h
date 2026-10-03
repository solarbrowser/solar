#pragma once

// The few socket calls the test servers make, the same on every platform.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <mutex>

namespace solar::test::sock {

#ifdef _WIN32
using Handle = SOCKET;
inline constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle kInvalid = -1;
#endif

inline void Init() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

inline void Close(Handle handle) {
#ifdef _WIN32
  closesocket(handle);
#else
  ::close(handle);
#endif
}

inline void ShutdownBoth(Handle handle) {
#ifdef _WIN32
  shutdown(handle, SD_BOTH);
#else
  ::shutdown(handle, SHUT_RDWR);
#endif
}

// True when a read would not block, waiting up to `ms`.
inline bool WaitReadable(Handle handle, int ms) {
#ifdef _WIN32
  WSAPOLLFD waiting{handle, POLLRDNORM, 0};
  return WSAPoll(&waiting, 1, ms) > 0;
#else
  pollfd waiting{handle, POLLIN, 0};
  return ::poll(&waiting, 1, ms) > 0;
#endif
}

inline Handle Accept(Handle listener) { return ::accept(listener, nullptr, nullptr); }

inline long Recv(Handle handle, void* buffer, size_t length) {
  return static_cast<long>(::recv(handle, static_cast<char*>(buffer), static_cast<int>(length), 0));
}

inline long Send(Handle handle, const void* data, size_t length) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  return static_cast<long>(::send(handle, static_cast<const char*>(data), static_cast<int>(length), flags));
}

}  // namespace solar::test::sock
