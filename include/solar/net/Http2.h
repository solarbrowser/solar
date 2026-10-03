#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "solar/net/Http1Parser.h"
#include "solar/net/Loop.h"

struct nghttp2_session;

namespace solar::net::internal {

// Where one response goes. The calls come from inside Http2Session::Receive, so what handles them
// must not destroy the session.
class Http2Stream {
 public:
  virtual ~Http2Stream() = default;
  virtual void OnHead(const HttpResponseHead& head) = 0;
  // Valid only until this returns.
  virtual void OnBody(std::span<const uint8_t> data) = 0;
  // The response is complete.
  virtual void OnEnd() = 0;
  // The stream ended without a complete response. `retryable` means the server says it did not
  // act on the request at all.
  virtual void OnReset(const std::string& reason, bool retryable) = 0;
};

struct Http2Request {
  std::string method = "GET";
  std::string scheme;
  std::string authority;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;  // lower case names
  // Sent as the request's body, read from here as the server's flow-control windows allow. It is
  // shared, not copied, and must not change while the stream is open. Null for no body.
  std::shared_ptr<const std::string> body;
};

// A client's side of one HTTP/2 connection (RFC 9113), many requests at once over a transport
// that is already up. Framing, HPACK and flow control are nghttp2's; this is the part that turns
// its events into the same head, body and end that an HTTP/1.1 response gives. Server push is
// refused, and what a response may weigh is limited before it can be held in memory.
class Http2Session {
 public:
  explicit Http2Session(Transport& transport);
  ~Http2Session();

  Http2Session(const Http2Session&) = delete;
  Http2Session& operator=(const Http2Session&) = delete;

  // Sends the connection preface and our settings. False if the session could not be made.
  bool Start();
  // Gives the session what the connection delivered. An error message means the connection is
  // unusable; nghttp2 has queued the GOAWAY for it, and it is sent before this returns.
  std::optional<std::string> Receive(std::span<const uint8_t> data);

  // Sends a request and returns its stream's id, or a negative number when the session cannot take
  // it. `stream` must stay valid until it has had OnEnd or OnReset, or Reset.
  int32_t Submit(const Http2Request& request, Http2Stream& stream);
  // The caller has lost interest in a stream: it gets no further calls.
  void Reset(int32_t id);

  // Tells the server this connection takes no more requests. Streams in flight still finish.
  void Shutdown();

  size_t active() const { return streams_.size(); }
  // Whether a new stream may start on this connection now.
  bool Available() const;
  // Nothing more can happen on it: it is going away and its streams are done.
  bool Finished() const;

 private:
  struct StreamState;
  struct Callbacks;

  void Flush();

  Transport& transport_;
  nghttp2_session* session_ = nullptr;
  std::unordered_map<int32_t, std::unique_ptr<StreamState>> streams_;
  bool receiving_ = false;
  bool flushing_ = false;
};

}  // namespace solar::net::internal
