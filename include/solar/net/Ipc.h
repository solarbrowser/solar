/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "solar/net/PageFetch.h"
#include "solar/net/WebSocketTypes.h"
#include "solar/url/Url.h"

// The messages between a page (a renderer) and the network service, as bytes: what crosses a process is copied, and a renderer is not trusted, so what is read is checked (a length
// larger than what is left, a number out of its range, an address that is none, all end the reading, and the one who sent it is let go of).
namespace solar::net::ipc {

// Writes little-endian numbers and length-prefixed bytes.
class Writer {
 public:
  void U8(uint8_t v) { bytes_.push_back(static_cast<char>(v)); }
  void U32(uint32_t v);
  void U64(uint64_t v);
  void Bool(bool v) { U8(v ? 1 : 0); }
  void String(std::string_view text);
  void Url(const url::Url& address);
  void Headers(const std::vector<std::pair<std::string, std::string>>& headers);
  const std::string& bytes() const { return bytes_; }
  std::string Take() { return std::move(bytes_); }

 private:
  std::string bytes_;
};

// Reads what a Writer wrote. Once a read has gone wrong (the bytes ended, a length was too large) every read after it gives nothing and ok() is false.
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  uint8_t U8();
  uint32_t U32();
  uint64_t U64();
  bool Bool() { return U8() != 0; }
  // The enumerator (0 .. last) a number stands for, or the first when it is more.
  template <typename Enum>
  Enum EnumUpTo(Enum last) {
    const uint32_t value = U8();
    if (value > static_cast<uint32_t>(last)) { ok_ = false; return static_cast<Enum>(0); }
    return static_cast<Enum>(value);
  }
  std::string String(size_t limit = size_t{1} << 28);
  std::optional<url::Url> ReadUrl();
  std::vector<std::pair<std::string, std::string>> Headers();
  bool ok() const { return ok_; }
  bool AtEnd() const { return pos_ == bytes_.size(); }

 private:
  std::string_view bytes_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// The messages. What a page says to the service, and what the service says back.
enum class Message : uint8_t {
  StartFetch = 1, Cancel, WebSocketConnect, WebSocketSend, WebSocketClose, WebSocketAbort,
  FetchResponse = 32, FetchBody, FetchEnd, FetchError, FetchRestart,
  WebSocketOpen = 40, WebSocketMessage, WebSocketClosed,
};

void Write(Writer& out, const PageRequest& request);
// False where what was read is not a request (nothing is then in `request` to be used).
bool Read(Reader& in, PageRequest& request);
void Write(Writer& out, const PageResponse& response);
bool Read(Reader& in, PageResponse& response);
void Write(Writer& out, const WebSocketOptions& options);
bool Read(Reader& in, WebSocketOptions& options);

// A connection of two ends that send messages (a type and bytes) in order, whole: a socket of a pair, for a thread or a process. Sending is safe from any thread, receiving is for one.
class Channel {
 public:
  // Takes the descriptor.
  explicit Channel(int fd);
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // False where the other end has gone, or the message is too large.
  bool Send(Message type, std::string_view bytes);
  // Waits for the next message. False at the end of the connection (or when it was Shutdown), or for a message that is too large.
  bool Receive(Message& type, std::string& bytes);
  // Wakes a Receive that waits, and ends the connection: for the thread that receives to be joined.
  void Shutdown();
  // A pair of connected descriptors, or -1 -1.
  static std::pair<int, int> MakePair();

  static constexpr size_t kMaxMessage = size_t{64} << 20;

 private:
  int fd_;
  std::mutex sendMutex_;
};

}  // namespace solar::net::ipc
