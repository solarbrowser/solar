/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/RemoteGl.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "solar/gui/FrameBuffer.h"
#include "solar/gui/GlWire.h"

namespace solar::gui {

namespace {

using namespace paint::gl;
using net::ipc::Reader;
using net::ipc::Writer;

constexpr size_t kFlushAt = size_t{1} << 20;

class Remote final : public Backend {
 public:
  explicit Remote(int fd) : channel_(fd) {
    reader_ = std::thread([this] {
      uint8_t type = 0;
      std::string bytes;
      while (channel_.ReceiveRaw(type, bytes)) {
        Reader in(bytes);
        const uint64_t request = in.U64();
        const bool ok = in.Bool();
        if (!in.ok()) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pending_.find(request);
        if (found == pending_.end()) continue;
        found->second.done = true;
        found->second.ok = ok;
        found->second.payload = bytes.substr(9);
        changed_.notify_all();
      }
      std::lock_guard<std::mutex> lock(mutex_);
      broken_ = true;
      changed_.notify_all();
    });
  }
  ~Remote() override {
    channel_.Shutdown();
    reader_.join();
  }

  uint32_t Create(int version, const std::string& attributes, int width, int height) override {
    width = std::clamp(width, 1, glwire::kMostSide);
    height = std::clamp(height, 1, glwire::kMostSide);
    Writer out;
    out.I32(version);
    out.String(attributes);
    out.I32(width);
    out.I32(height);
    std::string payload;
    if (!Ask(glwire::ToGl::Create, out, payload)) return 0;
    Reader in(payload);
    const uint32_t id = in.U32();
    if (!in.ok() || id == 0) return 0;
    Held held;
    held.width = width;
    held.height = height;
    const size_t at = attributes.find("\"premultipliedAlpha\":");
    held.premultiplied = at == std::string::npos || attributes.compare(at + 21, 4, "true") == 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      contexts_[id] = std::move(held);
    }
    if (!Announce(id, width, height)) return 0;
    return id;
  }

  std::string Info(uint32_t id) override {
    Writer out;
    out.U32(id);
    std::string payload;
    if (!Ask(glwire::ToGl::Info, out, payload)) return "";
    Reader in(payload);
    return in.String(size_t{1} << 24);
  }

  void Resize(uint32_t id, int width, int height) override {
    width = std::clamp(width, 1, glwire::kMostSide);
    height = std::clamp(height, 1, glwire::kMostSide);
    Writer out;
    out.U32(id);
    out.I32(width);
    out.I32(height);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      FlushLocked();
      channel_.SendRaw(static_cast<uint8_t>(glwire::ToGl::Resize), out.bytes());
    }
    Announce(id, width, height);
  }

  bool Premultiplied(uint32_t id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contexts_.find(id);
    return found == contexts_.end() || found->second.premultiplied;
  }

  std::vector<uint8_t> Present(uint32_t id, int& width, int& height) override {
    Writer out;
    out.U32(id);
    std::string payload;
    if (!Ask(glwire::ToGl::Present, out, payload)) return {};
    Reader in(payload);
    width = in.I32();
    height = in.I32();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contexts_.find(id);
    if (!in.ok() || found == contexts_.end() || width != found->second.width || height != found->second.height) return {};
    const size_t size = static_cast<size_t>(width) * height * 4;
    if (!found->second.memory.data() || found->second.memory.size() < size) return {};
    return std::vector<uint8_t>(found->second.memory.data(), found->second.memory.data() + size);
  }

  void Destroy(uint32_t id) override {
    Writer out;
    out.U32(id);
    std::lock_guard<std::mutex> lock(mutex_);
    FlushLocked();
    channel_.SendRaw(static_cast<uint8_t>(glwire::ToGl::Destroy), out.bytes());
    contexts_.erase(id);
  }

  bool Publish(uint32_t id) override {
    if (!CompositionEnabled()) return false;
    Writer out;
    out.U32(id);
    std::string payload;
    const bool ok = Ask(glwire::ToGl::Publish, out, payload);
    if (!ok && std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gl: the GPU's process did not publish context %u\n", id);
    return ok;
  }

  Result Call(uint32_t id, size_t operation, Args args, bool answer) override {
    const auto& names = OperationNames();
    if (operation >= names.size()) return {};
    glwire::Call call;
    call.context = id;
    call.operation = static_cast<uint32_t>(operation);
    call.args.assign(args.begin(), args.end());
    Writer out;
    std::unique_lock<std::mutex> lock(mutex_);
    if (broken_) return {};
    if (answer) call.request = nextRequest_++;
    glwire::WriteCall(out, call, names[operation]);
    batch_ += out.bytes();
    ++batchCount_;
    if (!answer) {
      if (batch_.size() >= kFlushAt) FlushLocked();
      return {};
    }
    Pending& pending = pending_[call.request];
    FlushLocked();
    const uint64_t request = call.request;
    changed_.wait_for(lock, std::chrono::seconds(60), [&] { return broken_ || pending_[request].done; });
    Pending done = std::move(pending_[request]);
    pending_.erase(request);
    if (!done.done || !done.ok) return {};
    Reader in(done.payload);
    Result result;
    std::string outBytes;
    if (!ReadResult(in, result, outBytes)) return {};
    const int output = glwire::OutputArgument(names[operation]);
    if (output >= 0 && static_cast<size_t>(output) < args.size() && args[output].writable && !outBytes.empty()) {
      std::memcpy(args[output].data, outBytes.data(), std::min(outBytes.size(), args[output].size));
    }
    return result;
  }

 private:
  struct Held {
    SharedMemory memory;
    int width = 0, height = 0;
    bool premultiplied = true;
  };
  struct Pending {
    bool done = false, ok = false;
    std::string payload;
  };

  static bool ReadResult(Reader& in, Result& r, std::string& bytes) { return glwire::ReadResult(in, r, bytes); }

  // What was sent on goes now (with the lock).
  void FlushLocked() {
    if (batchCount_ == 0) return;
    Writer head;
    head.U32(batchCount_);
    std::string bytes = head.Take();
    bytes += batch_;
    channel_.SendRaw(static_cast<uint8_t>(glwire::ToGl::Calls), bytes);
    batch_.clear();
    batchCount_ = 0;
  }

  // A request that is waited for: what the other end says is in `payload`.
  bool Ask(glwire::ToGl type, const Writer& body, std::string& payload) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (broken_) return false;
    const uint64_t request = nextRequest_++;
    pending_[request];
    FlushLocked();
    Writer out;
    out.U64(request);
    std::string bytes = out.Take();
    bytes += body.bytes();
    if (!channel_.SendRaw(static_cast<uint8_t>(type), bytes)) {
      pending_.erase(request);
      return false;
    }
    changed_.wait_for(lock, std::chrono::seconds(60), [&] { return broken_ || pending_[request].done; });
    Pending done = std::move(pending_[request]);
    pending_.erase(request);
    if (!done.done || !done.ok) return false;
    payload = std::move(done.payload);
    return true;
  }

  // The memory the GPU's process puts the pictures of a context in.
  bool Announce(uint32_t id, int width, int height) {
    SharedMemory memory;
    if (!memory.Create(static_cast<size_t>(width) * height * 4)) return false;
    Writer out;
    out.U32(id);
    out.I32(width);
    out.I32(height);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contexts_.find(id);
    if (found == contexts_.end()) return false;
    if (!channel_.SendRaw(static_cast<uint8_t>(glwire::ToGl::Buffer), out.bytes(), memory.fd())) return false;
    found->second.memory = std::move(memory);
    found->second.width = width;
    found->second.height = height;
    return true;
  }

  net::ipc::Channel channel_;
  std::thread reader_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool broken_ = false;
  uint64_t nextRequest_ = 1;
  std::map<uint64_t, Pending> pending_;
  std::string batch_;
  uint32_t batchCount_ = 0;
  std::map<uint32_t, Held> contexts_;
};

}  // namespace

std::unique_ptr<paint::gl::Backend> MakeRemoteGlBackend(int fd) { return std::make_unique<Remote>(fd); }

}  // namespace solar::gui
