#pragma once

// What differs between the platforms' sockets, in one place: the handle type, the error codes
// and a few calls. Everything else in the network layer is written against this.

#include <string>

#include "solar/net/Loop.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace solar::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Makes sockets usable: WSAStartup on Windows, nothing elsewhere. Safe to call repeatedly.
void InitializeSockets();

// The last error of a socket call on this thread: errno, or WSAGetLastError.
int LastSocketError();

void CloseSocket(SocketHandle socket);
bool SetNonBlocking(SocketHandle socket);

// The platform's text for an error code, which also covers kProtocolError.
std::string ErrorMessage(int error);

// Fills `storage` for `address` and returns its length.
int ToSockaddr(const SocketAddress& address, sockaddr_storage& storage);

}  // namespace solar::net
