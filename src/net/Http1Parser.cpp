#include "solar/net/Http1Parser.h"

#include <algorithm>
#include <limits>

namespace solar::net {

namespace {

constexpr size_t kMaxHeadSize = 64 * 1024;
constexpr size_t kMaxLineSize = 4 * 1024;
constexpr size_t kMaxTrailerSize = 64 * 1024;

bool IsTokenChar(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> SplitCommas(std::string_view s) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      parts.push_back(TrimOws(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  return parts;
}

// A header value may hold HTAB and printable bytes; any other control byte is refused.
bool ValidFieldValue(std::string_view value) {
  for (char c : value) {
    unsigned char u = static_cast<unsigned char>(c);
    if ((u < 0x20 && u != '\t') || u == 0x7F) return false;
  }
  return true;
}

}  // namespace

std::optional<std::string_view> HttpResponseHead::Header(std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (EqualsIgnoreCase(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

Http1ResponseParser::Http1ResponseParser(Sink& sink, bool headRequest) : sink_(sink), headRequest_(headRequest) {}

void Http1ResponseParser::Complete() {
  state_ = State::Done;
  sink_.OnComplete();
}

std::optional<std::string> Http1ResponseParser::Fail(std::string message) {
  state_ = State::Failed;
  return message;
}

std::optional<std::string> Http1ResponseParser::ParseHead(std::string_view head) {
  // Every LF must follow a CR and every CR must precede an LF; NUL is never valid.
  for (size_t i = 0; i < head.size(); ++i) {
    if (head[i] == '\0') return Fail("NUL byte in the response head");
    if (head[i] == '\n' && (i == 0 || head[i - 1] != '\r')) return Fail("bare LF in the response head");
    if (head[i] == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n')) return Fail("bare CR in the response head");
  }

  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start < head.size()) {
    size_t end = head.find("\r\n", start);
    lines.push_back(head.substr(start, end - start));
    start = end + 2;
  }
  // The head ends with an empty line, which is the last entry or absent after splitting.
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  if (lines.empty()) return Fail("empty response head");

  std::string_view statusLine = lines[0];
  if (!statusLine.starts_with("HTTP/1.0 ") && !statusLine.starts_with("HTTP/1.1 ") &&
      statusLine != "HTTP/1.0" && statusLine != "HTTP/1.1") {
    return Fail("not an HTTP/1.x status line");
  }
  if (statusLine.size() < 12 || !IsDigit(statusLine[9]) || !IsDigit(statusLine[10]) || !IsDigit(statusLine[11])) {
    return Fail("malformed status code");
  }
  if (statusLine.size() > 12 && statusLine[12] != ' ') return Fail("malformed status line");

  response_ = HttpResponseHead{};
  response_.minorVersion = statusLine[7] - '0';
  response_.status = (statusLine[9] - '0') * 100 + (statusLine[10] - '0') * 10 + (statusLine[11] - '0');
  if (response_.status < 100) return Fail("status code below 100");
  if (statusLine.size() > 13) response_.reason = std::string(statusLine.substr(13));

  for (size_t i = 1; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (line.empty()) return Fail("empty line inside the response head");
    if (line.front() == ' ' || line.front() == '\t') return Fail("folded header line");

    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return Fail("malformed header line");
    std::string_view name = line.substr(0, colon);
    if (!std::all_of(name.begin(), name.end(), IsTokenChar)) return Fail("invalid character in a header name");

    std::string_view value = TrimOws(line.substr(colon + 1));
    if (!ValidFieldValue(value)) return Fail("invalid character in a header value");
    response_.headers.emplace_back(std::string(name), std::string(value));
  }
  return std::nullopt;
}

// Decides how the body is delimited (RFC 9112 section 6.3) once the head is known.
std::optional<std::string> Http1ResponseParser::ChooseFraming() {
  const int status = response_.status;
  if (headRequest_ || status == 204 || status == 304) {
    Complete();
    return std::nullopt;
  }

  bool hasTransferEncoding = false;
  bool chunked = false;
  std::vector<std::string_view> contentLengths;
  for (const auto& [name, value] : response_.headers) {
    if (EqualsIgnoreCase(name, "transfer-encoding")) {
      hasTransferEncoding = true;
      for (std::string_view coding : SplitCommas(value)) {
        if (coding.empty()) continue;
        if (chunked) return Fail("a transfer coding follows chunked");
        if (!EqualsIgnoreCase(coding, "chunked")) return Fail("unsupported transfer coding");
        chunked = true;
      }
    } else if (EqualsIgnoreCase(name, "content-length")) {
      for (std::string_view part : SplitCommas(value)) contentLengths.push_back(part);
    }
  }

  if (hasTransferEncoding) {
    if (!contentLengths.empty()) return Fail("both Transfer-Encoding and Content-Length");
    if (!chunked) return Fail("Transfer-Encoding without chunked");
    state_ = State::ChunkSize;
    return std::nullopt;
  }

  if (!contentLengths.empty()) {
    std::optional<uint64_t> length;
    for (std::string_view part : contentLengths) {
      if (part.empty() || !std::all_of(part.begin(), part.end(), IsDigit)) return Fail("invalid Content-Length");
      uint64_t value = 0;
      for (char c : part) {
        if (value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) return Fail("Content-Length overflows");
        value = value * 10 + (c - '0');
      }
      if (length && *length != value) return Fail("Content-Length values disagree");
      length = value;
    }
    remaining_ = *length;
    if (remaining_ == 0) {
      Complete();
    } else {
      state_ = State::FixedBody;
    }
    return std::nullopt;
  }

  state_ = State::UntilClose;
  closeDelimited_ = true;
  return std::nullopt;
}

std::optional<std::string> Http1ResponseParser::Feed(std::span<const uint8_t> data) {
  if (state_ == State::Failed) return "the parser has already failed";
  if (!data.empty()) receivedAnything_ = true;

  while (true) {
    if (state_ == State::Done) {
      if (!data.empty()) return Fail("data after the end of the response");
      return std::nullopt;
    }
    if (data.empty()) return std::nullopt;

    switch (state_) {
      case State::Head: {
        // Only the head's own bytes are gathered; whatever follows the blank line is the body
        // and stays where it is.
        size_t used = 0;
        bool found = false;
        while (used < data.size() && !found) {
          head_.push_back(static_cast<char>(data[used++]));
          found = head_.size() >= 4 && head_.ends_with("\r\n\r\n");
          if (head_.size() > kMaxHeadSize) return Fail("response head too large");
        }
        data = data.subspan(used);
        if (!found) return std::nullopt;

        std::string head = std::move(head_);
        head_.clear();
        if (auto error = ParseHead(head)) return error;

        if (response_.status == 101) return Fail("protocol upgrade is not supported");
        if (response_.status >= 100 && response_.status < 200) {
          // An interim response (100 Continue, 103 Early Hints): the final one follows.
          response_ = HttpResponseHead{};
          break;
        }
        sink_.OnHead(response_);
        if (auto error = ChooseFraming()) return error;
        break;
      }

      case State::FixedBody: {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining_, data.size()));
        sink_.OnBody(data.first(take));
        data = data.subspan(take);
        remaining_ -= take;
        if (remaining_ == 0) Complete();
        break;
      }

      case State::UntilClose:
        sink_.OnBody(data);
        data = {};
        break;

      case State::ChunkSize: {
        // Gather the size line byte by byte; it is short, and this keeps the straddling simple.
        while (!data.empty()) {
          const char c = static_cast<char>(data.front());
          data = data.subspan(1);
          line_.push_back(c);
          if (line_.size() > kMaxLineSize) return Fail("chunk size line too long");
          if (c != '\n') continue;

          if (line_.size() < 2 || line_[line_.size() - 2] != '\r') return Fail("bare LF in a chunk size line");
          std::string_view size(line_.data(), line_.size() - 2);
          const size_t extension = size.find(';');
          if (extension != std::string_view::npos) size = size.substr(0, extension);
          if (size.empty() || size.size() > 16) return Fail("invalid chunk size");

          uint64_t value = 0;
          for (char digit : size) {
            const int h = HexValue(digit);
            if (h < 0) return Fail("invalid chunk size");
            value = value * 16 + static_cast<uint64_t>(h);
          }
          line_.clear();
          if (value == 0) {
            state_ = State::Trailers;
            trailerBytes_ = 0;
          } else {
            remaining_ = value;
            state_ = State::ChunkData;
          }
          break;
        }
        break;
      }

      case State::ChunkData: {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining_, data.size()));
        sink_.OnBody(data.first(take));
        data = data.subspan(take);
        remaining_ -= take;
        if (remaining_ == 0) {
          state_ = State::ChunkDataEnd;
          chunkEndSeen_ = 0;
        }
        break;
      }

      case State::ChunkDataEnd: {
        const char expected = chunkEndSeen_ == 0 ? '\r' : '\n';
        if (static_cast<char>(data.front()) != expected) return Fail("chunk data not followed by CRLF");
        data = data.subspan(1);
        if (++chunkEndSeen_ == 2) state_ = State::ChunkSize;
        break;
      }

      case State::Trailers: {
        while (!data.empty()) {
          const char c = static_cast<char>(data.front());
          data = data.subspan(1);
          line_.push_back(c);
          if (++trailerBytes_ > kMaxTrailerSize) return Fail("trailers too large");
          if (c != '\n') continue;

          if (line_.size() < 2 || line_[line_.size() - 2] != '\r') return Fail("bare LF in the trailers");
          const bool blank = line_.size() == 2;
          if (!blank) {
            std::string_view line(line_.data(), line_.size() - 2);
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return Fail("malformed trailer line");
            std::string_view name = line.substr(0, colon);
            if (!std::all_of(name.begin(), name.end(), IsTokenChar)) return Fail("invalid character in a trailer name");
            if (!ValidFieldValue(line.substr(colon + 1))) return Fail("invalid character in a trailer value");
          }
          line_.clear();
          if (blank) {
            Complete();
            break;
          }
        }
        break;
      }

      case State::Done:
      case State::Failed:
        return std::nullopt;  // handled before the switch
    }
  }
}

std::optional<std::string> Http1ResponseParser::Finish() {
  switch (state_) {
    case State::UntilClose:
      Complete();
      return std::nullopt;
    case State::Head:
      return Fail(receivedAnything_ ? "connection closed in the middle of the response head"
                                    : "connection closed before any response");
    case State::Failed:
    case State::Done:
      return std::nullopt;
    default:
      return Fail("connection closed before the response body ended");
  }
}

}  // namespace solar::net
