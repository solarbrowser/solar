#include "solar/net/LoopCore.h"

#ifdef __linux__

#include <liburing.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace solar::net::internal {

namespace {

constexpr unsigned kRingEntries = 256;
constexpr unsigned kBufferCount = 256;  // a power of two, as the buffer ring requires
constexpr unsigned kBufferSize = 16 * 1024;
constexpr int kBufferGroup = 0;
constexpr unsigned kCompletionBatch = 64;

enum class Op { Connect, Recv, Send, Cancel, Wake };

struct Data;

// What an in-flight operation's user_data points at, so a completion finds its connection.
struct Tag {
  ConnectionState* connection = nullptr;
  Data* data = nullptr;
  Op op = Op::Connect;
};

struct Data {
  Tag connectTag, recvTag, sendTag, cancelTag;
  // Operations submitted whose final completion has not arrived. Nothing is freed before 0.
  int inflight = 0;
  bool connectActive = false;
  bool recvActive = false;
  bool sendActive = false;
  sockaddr_storage address{};
  int addressLength = 0;

  explicit Data(ConnectionState* c) {
    connectTag = {c, this, Op::Connect};
    recvTag = {c, this, Op::Recv};
    sendTag = {c, this, Op::Send};
    cancelTag = {c, this, Op::Cancel};
  }
};

Data& Of(ConnectionState& c) { return *static_cast<Data*>(c.backendData); }

class UringBackend final : public Backend {
 public:
  explicit UringBackend(Loop::Impl& core) : core_(core) {}

  ~UringBackend() override {
    if (wake_[0] >= 0) {
      ::close(wake_[0]);
      ::close(wake_[1]);
    }
    if (!initialized_) return;
    io_uring_free_buf_ring(&ring_, bufferRing_, kBufferCount, kBufferGroup);
    io_uring_queue_exit(&ring_);
    std::free(buffers_);
  }

  // Receiving into a ring of buffers the kernel picks from needs a kernel that has them; false if
  // this one does not.
  bool Initialize() {
    if (io_uring_queue_init(kRingEntries, &ring_, 0) < 0) return false;
    int error = 0;
    bufferRing_ = io_uring_setup_buf_ring(&ring_, kBufferCount, kBufferGroup, 0, &error);
    if (!bufferRing_) {
      io_uring_queue_exit(&ring_);
      return false;
    }
    initialized_ = true;
    buffers_ = static_cast<uint8_t*>(std::aligned_alloc(4096, size_t{kBufferCount} * kBufferSize));
    for (unsigned id = 0; id < kBufferCount; ++id) {
      io_uring_buf_ring_add(bufferRing_, buffers_ + size_t{id} * kBufferSize, kBufferSize, static_cast<unsigned short>(id),
                            io_uring_buf_ring_mask(kBufferCount), static_cast<int>(id));
    }
    io_uring_buf_ring_advance(bufferRing_, kBufferCount);

    if (!MakeWakePipe(wake_)) return false;
    ArmWake();
    return true;
  }

  void Wake() override {
    const char byte = 1;
    // A full pipe already has a wake-up in it, so a failed write loses nothing.
    [[maybe_unused]] const ssize_t written = ::write(wake_[1], &byte, 1);
  }

  const char* Name() const override { return "io_uring"; }

  void Connect(ConnectionState& c) override {
    auto* data = new Data(&c);
    c.backendData = data;
    data->addressLength = ToSockaddr(c.address, data->address);

    c.socket = ::socket(data->address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c.socket < 0) {
      core_.Connected(c, LastSocketError());  // reported from Run, like every other failure
      return;
    }
    int one = 1;
    ::setsockopt(c.socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    io_uring_sqe* sqe = NextSqe();
    io_uring_prep_connect(sqe, c.socket, reinterpret_cast<const sockaddr*>(&data->address), static_cast<socklen_t>(data->addressLength));
    io_uring_sqe_set_data(sqe, &data->connectTag);
    data->connectActive = true;
    ++data->inflight;
  }

  void StartReceive(ConnectionState& c) override { StartRecv(c); }

  void WantSend(ConnectionState& c) override {
    if (!Of(c).sendActive) StartSend(c);
  }

  void Close(ConnectionState& c) override {
    Data& d = Of(c);
    if (c.socket >= 0) ::shutdown(c.socket, SHUT_RDWR);
    if (d.connectActive) Cancel(c, d.connectTag);
    if (d.recvActive) Cancel(c, d.recvTag);
    if (d.inflight == 0) core_.Quiescent(c);
  }

  void Release(ConnectionState& c) override {
    if (c.socket >= 0) ::close(c.socket);
    delete static_cast<Data*>(c.backendData);
    c.backendData = nullptr;
  }

  bool Wait(int timeoutMs) override {
    io_uring_submit(&ring_);

    io_uring_cqe* first = nullptr;
    int waited;
    if (timeoutMs < 0) {
      waited = io_uring_wait_cqe(&ring_, &first);
    } else {
      __kernel_timespec timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
      waited = io_uring_wait_cqe_timeout(&ring_, &first, &timeout);
    }
    if (waited == -ETIME || waited == -EINTR) return true;
    if (waited < 0) return false;

    io_uring_cqe* batch[kCompletionBatch];
    const unsigned count = io_uring_peek_batch_cqe(&ring_, batch, kCompletionBatch);
    for (unsigned i = 0; i < count; ++i) Handle(*batch[i]);
    io_uring_cq_advance(&ring_, count);
    return true;
  }

 private:
  io_uring_sqe* NextSqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      io_uring_submit(&ring_);
      sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
  }

  void RecycleBuffer(unsigned id) {
    io_uring_buf_ring_add(bufferRing_, buffers_ + size_t{id} * kBufferSize, kBufferSize, static_cast<unsigned short>(id),
                          io_uring_buf_ring_mask(kBufferCount), 0);
    io_uring_buf_ring_advance(bufferRing_, 1);
  }

  void StartRecv(ConnectionState& c) {
    Data& d = Of(c);
    io_uring_sqe* sqe = NextSqe();
    io_uring_prep_recv_multishot(sqe, c.socket, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kBufferGroup;
    io_uring_sqe_set_data(sqe, &d.recvTag);
    d.recvActive = true;
    ++d.inflight;
  }

  void StartSend(ConnectionState& c) {
    Data& d = Of(c);
    const std::string_view pending = core_.PendingSend(c);
    io_uring_sqe* sqe = NextSqe();
    io_uring_prep_send(sqe, c.socket, pending.data(), pending.size(), MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &d.sendTag);
    d.sendActive = true;
    ++d.inflight;
  }

  void Cancel(ConnectionState& c, Tag& target) {
    Data& d = Of(c);
    io_uring_sqe* sqe = NextSqe();
    io_uring_prep_cancel64(sqe, reinterpret_cast<uint64_t>(&target), 0);
    io_uring_sqe_set_data(sqe, &d.cancelTag);
    ++d.inflight;
  }

  void ArmWake() {
    io_uring_sqe* sqe = NextSqe();
    io_uring_prep_read(sqe, wake_[0], wakeBuffer_, sizeof(wakeBuffer_), 0);
    io_uring_sqe_set_data(sqe, &wakeTag_);
  }

  void Handle(const io_uring_cqe& cqe) {
    Tag* tag = static_cast<Tag*>(io_uring_cqe_get_data(&cqe));
    if (tag->op == Op::Wake) {
      ArmWake();  // what woke the loop is drained; Run does what was posted
      return;
    }
    ConnectionState& c = *tag->connection;
    Data& d = *tag->data;
    const int result = cqe.res;
    const unsigned flags = cqe.flags;

    switch (tag->op) {
      case Op::Connect:
        d.connectActive = false;
        --d.inflight;
        core_.Connected(c, result < 0 ? -result : 0);
        break;

      case Op::Recv: {
        const bool more = flags & IORING_CQE_F_MORE;
        if (!more) {
          d.recvActive = false;
          --d.inflight;
        }
        if (result > 0 && (flags & IORING_CQE_F_BUFFER)) {
          const unsigned id = flags >> IORING_CQE_BUFFER_SHIFT;
          core_.Received(c, std::span<const uint8_t>(buffers_ + size_t{id} * kBufferSize, static_cast<size_t>(result)));
          RecycleBuffer(id);
        } else if (result == 0) {
          core_.PeerClosed(c);
        } else if (result < 0 && result != -ENOBUFS && result != -ECANCELED) {
          core_.ReceiveFailed(c, -result);
        }
        // A multishot receive also stops when the buffer ring runs dry; ask for another.
        if (!more && !c.closing) StartRecv(c);
        break;
      }

      case Op::Send:
        d.sendActive = false;
        --d.inflight;
        if (c.closing) break;
        if (result < 0) {
          core_.SendFailed(c, -result);
          break;
        }
        core_.Sent(c, static_cast<size_t>(result));
        if (!c.sendQueue.empty()) StartSend(c);
        break;

      case Op::Cancel:
        --d.inflight;
        break;

      case Op::Wake:
        break;
    }

    if (c.closing && d.inflight == 0) core_.Quiescent(c);
  }

  Loop::Impl& core_;
  io_uring ring_{};
  io_uring_buf_ring* bufferRing_ = nullptr;
  uint8_t* buffers_ = nullptr;
  bool initialized_ = false;
  int wake_[2] = {-1, -1};
  char wakeBuffer_[64];
  Tag wakeTag_{nullptr, nullptr, Op::Wake};
};

}  // namespace

std::unique_ptr<Backend> MakeUringBackend(Loop::Impl& core) {
  auto backend = std::make_unique<UringBackend>(core);
  if (!backend->Initialize()) return nullptr;
  return backend;
}

}  // namespace solar::net::internal

#else

namespace solar::net::internal {
std::unique_ptr<Backend> MakeUringBackend(Loop::Impl&) { return nullptr; }
}  // namespace solar::net::internal

#endif
