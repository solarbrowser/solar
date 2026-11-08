/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/Ipc.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::net::ipc {

void Writer::U32(uint32_t v) {
  for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void Writer::U64(uint64_t v) {
  for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void Writer::String(std::string_view text) {
  U32(static_cast<uint32_t>(text.size()));
  bytes_.append(text);
}
void Writer::Url(const url::Url& address) { String(url::Serialize(address)); }
void Writer::Headers(const std::vector<std::pair<std::string, std::string>>& headers) {
  U32(static_cast<uint32_t>(headers.size()));
  for (const auto& [name, value] : headers) {
    String(name);
    String(value);
  }
}

uint8_t Reader::U8() {
  if (!ok_ || pos_ + 1 > bytes_.size()) { ok_ = false; return 0; }
  return static_cast<uint8_t>(bytes_[pos_++]);
}
uint32_t Reader::U32() {
  if (!ok_ || pos_ + 4 > bytes_.size()) { ok_ = false; return 0; }
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
  pos_ += 4;
  return v;
}
uint64_t Reader::U64() {
  if (!ok_ || pos_ + 8 > bytes_.size()) { ok_ = false; return 0; }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
  pos_ += 8;
  return v;
}
std::string Reader::String(size_t limit) {
  const uint32_t size = U32();
  if (!ok_ || size > limit || pos_ + size > bytes_.size()) { ok_ = false; return {}; }
  std::string out(bytes_.substr(pos_, size));
  pos_ += size;
  return out;
}
std::optional<url::Url> Reader::ReadUrl() {
  const std::string text = String(1 << 20);
  if (!ok_) return std::nullopt;
  auto parsed = url::Parse(text);
  if (!parsed) ok_ = false;
  return parsed;
}
std::vector<std::pair<std::string, std::string>> Reader::Headers() {
  std::vector<std::pair<std::string, std::string>> out;
  const uint32_t count = U32();
  // (Each header is at least eight bytes: a count that cannot be true is not one.)
  if (!ok_ || count > (bytes_.size() - pos_) / 8) { ok_ = false; return out; }
  for (uint32_t i = 0; i < count && ok_; ++i) {
    std::string name = String(1 << 16);
    std::string value = String(1 << 20);
    out.emplace_back(std::move(name), std::move(value));
  }
  return out;
}

void Write(Writer& out, const PageRequest& request) {
  out.Url(request.url);
  out.String(request.method);
  out.Headers(request.headers);
  out.Bool(request.body != nullptr);
  if (request.body) out.String(*request.body);
  out.U8(static_cast<uint8_t>(request.destination));
  out.U8(static_cast<uint8_t>(request.mode));
  out.U8(static_cast<uint8_t>(request.credentials));
  out.U8(static_cast<uint8_t>(request.redirect));
  out.U8(static_cast<uint8_t>(request.cache));
  out.Bool(request.sendsReferrer);
  out.String(request.referrerPolicy);
  out.Url(request.pageUrl);
  out.U64(static_cast<uint64_t>(request.timeout.count()));
  out.Bool(request.restartable);
  out.U64(static_cast<uint64_t>(request.stall.count()));
}

bool Read(Reader& in, PageRequest& request) {
  auto address = in.ReadUrl();
  request.method = in.String(64);
  request.headers = in.Headers();
  if (in.Bool()) request.body = std::make_shared<const std::string>(in.String(size_t{1} << 30));
  request.destination = in.EnumUpTo(RequestDestination::Other);
  request.mode = in.EnumUpTo(RequestMode::Navigate);
  request.credentials = in.EnumUpTo(RequestCredentials::Include);
  const uint8_t redirect = in.U8();
  request.redirect = redirect <= 2 ? redirect : 0;
  request.cache = in.EnumUpTo(CacheMode::OnlyIfCached);
  request.sendsReferrer = in.Bool();
  request.referrerPolicy = in.String(64);
  auto page = in.ReadUrl();
  // (A request that asks for more time than a day, or a stall of none, is the service's to bound.)
  request.timeout = std::chrono::milliseconds(std::min<uint64_t>(in.U64(), uint64_t{24} * 3600 * 1000));
  request.restartable = in.Bool();
  request.stall = std::chrono::milliseconds(std::min<uint64_t>(in.U64(), uint64_t{600} * 1000));
  if (!in.ok() || !address || !page) return false;
  if (redirect > 2) return false;
  request.url = std::move(*address);
  request.pageUrl = std::move(*page);
  return true;
}

void Write(Writer& out, const PageResponse& response) {
  out.U8(static_cast<uint8_t>(response.type));
  out.U32(static_cast<uint32_t>(response.status));
  out.String(response.statusText);
  out.Headers(response.headers);
  out.Url(response.url);
  out.Bool(response.redirected);
  out.U32(static_cast<uint32_t>(response.hiddenStatus));
  out.String(response.hiddenLocation);
  out.Bool(response.hasBody);
}

bool Read(Reader& in, PageResponse& response) {
  response.type = in.EnumUpTo(ResponseType::OpaqueRedirect);
  response.status = static_cast<int>(in.U32());
  response.statusText = in.String(1 << 16);
  response.headers = in.Headers();
  auto address = in.ReadUrl();
  response.redirected = in.Bool();
  response.hiddenStatus = static_cast<int>(in.U32());
  response.hiddenLocation = in.String(1 << 20);
  response.hasBody = in.Bool();
  if (!in.ok() || !address) return false;
  response.url = std::move(*address);
  return true;
}

void Write(Writer& out, const WebSocketOptions& options) {
  out.U32(static_cast<uint32_t>(options.protocols.size()));
  for (const std::string& protocol : options.protocols) out.String(protocol);
  out.String(options.origin);
  out.Bool(options.initiator.has_value());
  if (options.initiator) out.Url(*options.initiator);
  out.Headers(options.headers);
  out.U64(static_cast<uint64_t>(options.maxMessageBytes));
}

bool Read(Reader& in, WebSocketOptions& options) {
  const uint32_t count = in.U32();
  if (!in.ok() || count > 1024) return false;
  for (uint32_t i = 0; i < count && in.ok(); ++i) options.protocols.push_back(in.String(256));
  options.origin = in.String(1 << 12);
  if (in.Bool()) {
    auto initiator = in.ReadUrl();
    if (!initiator) return false;
    options.initiator = std::move(*initiator);
  }
  options.headers = in.Headers();
  // (What the page asks for is bounded by what the service allows.)
  options.maxMessageBytes = static_cast<size_t>(std::min<uint64_t>(in.U64(), uint64_t{256} << 20));
  return in.ok();
}

Channel::Channel(int fd) : fd_(fd) {}

Channel::~Channel() {
  if (fd_ >= 0) ::close(fd_);
}

std::pair<int, int> Channel::MakePair() {
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return {-1, -1};
  return {fds[0], fds[1]};
}

void Channel::Shutdown() {
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

bool Channel::Send(Message type, std::string_view bytes) {
  if (bytes.size() > kMaxMessage) return false;
  std::lock_guard<std::mutex> lock(sendMutex_);
  if (fd_ < 0) return false;
  std::string frame;
  frame.reserve(5 + bytes.size());
  const uint32_t size = static_cast<uint32_t>(bytes.size());
  for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>((size >> (8 * i)) & 0xFF));
  frame.push_back(static_cast<char>(type));
  frame.append(bytes);
  size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool Channel::Receive(Message& type, std::string& bytes) {
  const auto readAll = [this](char* into, size_t size) {
    size_t got = 0;
    while (got < size) {
      const ssize_t n = ::recv(fd_, into + got, size - got, 0);
      if (n == 0) return false;
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      got += static_cast<size_t>(n);
    }
    return true;
  };
  char header[5];
  if (!readAll(header, 5)) return false;
  uint32_t size = 0;
  for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<uint8_t>(header[i])) << (8 * i);
  if (size > kMaxMessage) return false;
  type = static_cast<Message>(static_cast<uint8_t>(header[4]));
  bytes.resize(size);
  return size == 0 || readAll(bytes.data(), size);
}

}  // namespace solar::net::ipc
