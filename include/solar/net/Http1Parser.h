#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace solar::net {

struct HttpResponseHead {
  int minorVersion = 1;  // the 1 of HTTP/1.1
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;

  // The first header named `name`, compared without regard to case.
  std::optional<std::string_view> Header(std::string_view name) const;
};

// An incremental parser for an HTTP/1.1 response (RFC 9112). It gives the same result however
// the bytes are cut into Feed calls. Body bytes go to the sink as views into what was fed, so
// they are not copied; only the head, which is small, is gathered into a buffer of its own.
//
// It is strict where leniency has been a source of request smuggling: a bare CR or LF, a
// space before a header's colon, a folded header, and a message with both Content-Length and
// Transfer-Encoding or with Content-Lengths that disagree are all refused.
class Http1ResponseParser {
 public:
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void OnHead(const HttpResponseHead& head) = 0;
    // Valid only until this returns.
    virtual void OnBody(std::span<const uint8_t> data) = 0;
    virtual void OnComplete() = 0;
  };

  // `headRequest`: the response answers a HEAD request, so it has no body whatever it says.
  explicit Http1ResponseParser(Sink& sink, bool headRequest = false);

  // Returns an error message when the bytes are not a valid response; the parser is unusable
  // after that.
  std::optional<std::string> Feed(std::span<const uint8_t> data);
  // The connection ended. Completes a body that runs until close; otherwise a message that
  // is not finished is an error.
  std::optional<std::string> Finish();

  bool complete() const { return state_ == State::Done; }
  // True when the body ran until the connection closed, which leaves nothing to reuse.
  bool closeDelimited() const { return closeDelimited_; }

 private:
  enum class State { Head, FixedBody, ChunkSize, ChunkData, ChunkDataEnd, Trailers, UntilClose, Done, Failed };

  std::optional<std::string> ParseHead(std::string_view head);
  std::optional<std::string> ChooseFraming();
  std::optional<std::string> Fail(std::string message);
  void Complete();

  Sink& sink_;
  bool headRequest_;
  State state_ = State::Head;

  std::string head_;     // the head so far, until its blank line
  std::string line_;     // a chunk-size or trailer line so far
  size_t trailerBytes_ = 0;
  uint64_t remaining_ = 0;
  size_t chunkEndSeen_ = 0;  // how much of the CRLF after chunk data has been seen
  HttpResponseHead response_;
  bool receivedAnything_ = false;
  bool closeDelimited_ = false;
};

}  // namespace solar::net
