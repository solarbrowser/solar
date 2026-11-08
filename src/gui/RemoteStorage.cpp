/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "RemoteStorage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

#include "solar/gui/Wire.h"
#include "solar/web-api/Storage.h"

namespace solar::gui {

namespace {

using webapi::StorageArea;

class RemoteStorage final : public webapi::StorageBackend {
 public:
  explicit RemoteStorage(net::ipc::Channel& link) : link_(link) {}

  std::optional<std::string> Read(StorageArea area, const std::string& origin, const std::string& id, size_t maxBytes) override {
    wire::StorageRequest request;
    request.op = wire::StorageRequest::Op::Read;
    request.area = static_cast<uint8_t>(area);
    request.origin = origin;
    request.file = id;
    request.maxBytes = static_cast<uint32_t>(std::min<size_t>(maxBytes, wire::kMaxStorageFile));
    Reply reply;
    if (!Ask(request, reply) || !reply.ok) return std::nullopt;
    return std::move(reply.data);
  }
  bool Write(StorageArea area, const std::string& origin, const std::string& id, std::string_view bytes) override {
    if (bytes.size() > wire::kMaxStorageFile) return false;
    wire::StorageRequest request;
    request.op = wire::StorageRequest::Op::Write;
    request.area = static_cast<uint8_t>(area);
    request.origin = origin;
    request.file = id;
    request.data = std::string(bytes);
    Reply reply;
    return Ask(request, reply) && reply.ok;
  }
  bool Remove(StorageArea area, const std::string& origin, const std::string& id) override {
    wire::StorageRequest request;
    request.op = wire::StorageRequest::Op::Remove;
    request.area = static_cast<uint8_t>(area);
    request.origin = origin;
    request.file = id;
    Reply reply;
    return Ask(request, reply) && reply.ok;
  }
  std::vector<std::string> List(StorageArea area, const std::string& origin) override {
    wire::StorageRequest request;
    request.op = wire::StorageRequest::Op::List;
    request.area = static_cast<uint8_t>(area);
    request.origin = origin;
    Reply reply;
    std::vector<std::string> ids;
    if (!Ask(request, reply) || !reply.ok) return ids;
    size_t from = 0;
    while (from < reply.data.size()) {
      size_t end = reply.data.find('\n', from);
      if (end == std::string::npos) end = reply.data.size();
      ids.push_back(reply.data.substr(from, end - from));
      from = end + 1;
    }
    return ids;
  }

  struct Reply {
    bool arrived = false, ok = false;
    std::string data;
  };
  void Answer(uint32_t id, bool ok, std::string data) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = waiting_.find(id);
    if (found == waiting_.end()) return;
    found->second->arrived = true;
    found->second->ok = ok;
    found->second->data = std::move(data);
    changed_.notify_all();
  }

 private:
  // Asks and waits (the script that stores is waiting for the answer: a file is what it is). Where the browser does not answer in half a minute, or is gone, it is a failure.
  bool Ask(wire::StorageRequest& request, Reply& reply) {
    request.id = next_++;
    net::ipc::Writer out;
    wire::Write(out, request);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      waiting_[request.id] = &reply;
    }
    if (!link_.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Storage), out.bytes())) {
      std::lock_guard<std::mutex> lock(mutex_);
      waiting_.erase(request.id);
      return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::seconds(30), [&] { return reply.arrived; });
    waiting_.erase(request.id);
    return reply.arrived;
  }

  net::ipc::Channel& link_;
  std::atomic<uint32_t> next_{1};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::map<uint32_t, Reply*> waiting_;
};

RemoteStorage* g_remote = nullptr;

}  // namespace

void InstallRemoteStorage(net::ipc::Channel& link) {
  g_remote = new RemoteStorage(link);  // (it lives as long as the process: the scripts keep asking until the end)
  webapi::SetStorage(g_remote);
}

void DeliverStorageReply(const std::string& bytes) {
  if (!g_remote) return;
  net::ipc::Reader in(bytes);
  const uint32_t id = in.U32();
  const bool ok = in.Bool();
  std::string data = in.String(wire::kMaxStorageFile + 4096);
  if (in.ok()) g_remote->Answer(id, ok, std::move(data));
}

}  // namespace solar::gui
