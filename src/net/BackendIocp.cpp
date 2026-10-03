#include "LoopCore.h"

#ifdef _WIN32

#include <cstring>
#include <vector>

namespace solar::net::internal {

namespace {

constexpr size_t kReceiveBufferSize = 16 * 1024;
constexpr ULONG kCompletionBatch = 64;

enum class Kind { Connect, Recv, Send };

// What an operation's completion points back at. OVERLAPPED must be the first member: the
// completion port hands back its address.
struct Operation {
  OVERLAPPED overlapped;
  Kind kind;
  ConnectionState* connection;
};

struct Data {
  Operation connectOp{{}, Kind::Connect, nullptr};
  Operation recvOp{{}, Kind::Recv, nullptr};
  Operation sendOp{{}, Kind::Send, nullptr};

  // Operations started whose completion has not been taken. Nothing is freed before 0.
  int inflight = 0;
  bool connectActive = false;
  bool recvActive = false;
  bool sendActive = false;

  sockaddr_storage address{};
  int addressLength = 0;
  std::vector<char> receiveBuffer = std::vector<char>(kReceiveBufferSize);

  explicit Data(ConnectionState* c) {
    connectOp.connection = c;
    recvOp.connection = c;
    sendOp.connection = c;
  }
};

Data& Of(ConnectionState& c) { return *static_cast<Data*>(c.backendData); }

class IocpBackend final : public Backend {
 public:
  explicit IocpBackend(Loop::Impl& core) : core_(core) {}

  ~IocpBackend() override {
    if (port_) CloseHandle(port_);
  }

  bool Initialize() {
    port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!port_) return false;

    // ConnectEx is not exported by the library; it is asked for through the socket.
    SOCKET probe = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (probe == INVALID_SOCKET) return false;
    GUID id = WSAID_CONNECTEX;
    DWORD bytes = 0;
    const int result = WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &connectEx_, sizeof(connectEx_), &bytes, nullptr, nullptr);
    closesocket(probe);
    return result == 0 && connectEx_ != nullptr;
  }

  const char* Name() const override { return "iocp"; }

  void Connect(ConnectionState& c) override {
    auto* data = new Data(&c);
    c.backendData = data;
    data->addressLength = ToSockaddr(c.address, data->address);

    const int family = data->address.ss_family;
    c.socket = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (c.socket == INVALID_SOCKET) {
      core_.Connected(c, WSAGetLastError());  // reported from Run, like every other failure
      return;
    }
    BOOL one = TRUE;
    setsockopt(c.socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(c.socket), port_, 0, 0);

    // ConnectEx wants the socket bound first.
    sockaddr_storage any{};
    int anyLength;
    if (family == AF_INET6) {
      reinterpret_cast<sockaddr_in6*>(&any)->sin6_family = AF_INET6;
      anyLength = sizeof(sockaddr_in6);
    } else {
      reinterpret_cast<sockaddr_in*>(&any)->sin_family = AF_INET;
      anyLength = sizeof(sockaddr_in);
    }
    if (bind(c.socket, reinterpret_cast<const sockaddr*>(&any), anyLength) != 0) {
      core_.Connected(c, WSAGetLastError());
      return;
    }

    std::memset(&data->connectOp.overlapped, 0, sizeof(OVERLAPPED));
    const BOOL started = connectEx_(c.socket, reinterpret_cast<const sockaddr*>(&data->address), data->addressLength, nullptr, 0, nullptr,
                                    &data->connectOp.overlapped);
    if (!started && WSAGetLastError() != ERROR_IO_PENDING) {
      core_.Connected(c, WSAGetLastError());
      return;
    }
    data->connectActive = true;
    ++data->inflight;
  }

  void StartReceive(ConnectionState& c) override { PostReceive(c); }

  void WantSend(ConnectionState& c) override {
    if (!Of(c).sendActive) PostSend(c);
  }

  void Close(ConnectionState& c) override {
    Data& d = Of(c);
    if (c.socket != INVALID_SOCKET) {
      shutdown(c.socket, SD_BOTH);
      if (d.inflight > 0) CancelIoEx(reinterpret_cast<HANDLE>(c.socket), nullptr);
    }
    if (d.inflight == 0) core_.Quiescent(c);
  }

  void Release(ConnectionState& c) override {
    Data* d = static_cast<Data*>(c.backendData);
    if (c.socket != INVALID_SOCKET) {
      if (d && d->inflight > 0) CancelIoEx(reinterpret_cast<HANDLE>(c.socket), nullptr);
      closesocket(c.socket);
    }
    // Only when the loop is torn down with operations still out: the kernel must be done with
    // their OVERLAPPED structures before they are freed.
    while (d && d->inflight > 0) {
      OVERLAPPED_ENTRY entries[kCompletionBatch];
      ULONG count = 0;
      if (!GetQueuedCompletionStatusEx(port_, entries, kCompletionBatch, &count, 100, FALSE)) break;
      for (ULONG i = 0; i < count; ++i) {
        if (!entries[i].lpOverlapped) continue;
        Operation* op = CONTAINING_RECORD(entries[i].lpOverlapped, Operation, overlapped);
        --Of(*op->connection).inflight;
      }
    }
    delete d;
    c.backendData = nullptr;
  }

  bool Wait(int timeoutMs) override {
    OVERLAPPED_ENTRY entries[kCompletionBatch];
    ULONG count = 0;
    const DWORD timeout = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
    if (!GetQueuedCompletionStatusEx(port_, entries, kCompletionBatch, &count, timeout, FALSE)) {
      return GetLastError() == WAIT_TIMEOUT;
    }
    for (ULONG i = 0; i < count; ++i) {
      if (entries[i].lpOverlapped) Handle(*CONTAINING_RECORD(entries[i].lpOverlapped, Operation, overlapped));
    }
    return true;
  }

 private:
  void PostReceive(ConnectionState& c) {
    Data& d = Of(c);
    WSABUF buffer{static_cast<ULONG>(d.receiveBuffer.size()), d.receiveBuffer.data()};
    DWORD flags = 0;
    std::memset(&d.recvOp.overlapped, 0, sizeof(OVERLAPPED));
    if (WSARecv(c.socket, &buffer, 1, nullptr, &flags, &d.recvOp.overlapped, nullptr) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
      core_.ReceiveFailed(c, WSAGetLastError());
      return;
    }
    d.recvActive = true;
    ++d.inflight;
  }

  void PostSend(ConnectionState& c) {
    Data& d = Of(c);
    const std::string_view pending = core_.PendingSend(c);
    WSABUF buffer{static_cast<ULONG>(pending.size()), const_cast<char*>(pending.data())};
    std::memset(&d.sendOp.overlapped, 0, sizeof(OVERLAPPED));
    if (WSASend(c.socket, &buffer, 1, nullptr, 0, &d.sendOp.overlapped, nullptr) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
      core_.SendFailed(c, WSAGetLastError());
      return;
    }
    d.sendActive = true;
    ++d.inflight;
  }

  void Handle(Operation& op) {
    ConnectionState& c = *op.connection;
    Data& d = Of(c);

    DWORD bytes = 0;
    DWORD flags = 0;
    const BOOL succeeded = WSAGetOverlappedResult(c.socket, &op.overlapped, &bytes, FALSE, &flags);
    const int error = succeeded ? 0 : WSAGetLastError();

    switch (op.kind) {
      case Kind::Connect:
        d.connectActive = false;
        --d.inflight;
        if (!c.closing && error == 0) setsockopt(c.socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        core_.Connected(c, error);
        break;

      case Kind::Recv:
        d.recvActive = false;
        --d.inflight;
        if (c.closing) break;
        if (error != 0) {
          core_.ReceiveFailed(c, error);
        } else if (bytes == 0) {
          core_.PeerClosed(c);
        } else {
          core_.Received(c, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(d.receiveBuffer.data()), bytes));
          if (!c.closing) PostReceive(c);
        }
        break;

      case Kind::Send:
        d.sendActive = false;
        --d.inflight;
        if (c.closing) break;
        if (error != 0) {
          core_.SendFailed(c, error);
          break;
        }
        core_.Sent(c, bytes);
        if (!c.sendQueue.empty()) PostSend(c);
        break;
    }

    if (c.closing && d.inflight == 0) core_.Quiescent(c);
  }

  Loop::Impl& core_;
  HANDLE port_ = nullptr;
  LPFN_CONNECTEX connectEx_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> MakeIocpBackend(Loop::Impl& core) {
  auto backend = std::make_unique<IocpBackend>(core);
  if (!backend->Initialize()) return nullptr;
  return backend;
}

}  // namespace solar::net::internal

#else

namespace solar::net::internal {
std::unique_ptr<Backend> MakeIocpBackend(Loop::Impl&) { return nullptr; }
}  // namespace solar::net::internal

#endif
