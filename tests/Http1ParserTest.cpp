#include <cstdio>
#include <string>
#include <vector>

#include "solar/net/Http1Parser.h"

using namespace std::string_literals;

namespace {

using solar::net::Http1ResponseParser;
using solar::net::HttpResponseHead;

struct Outcome {
  bool failed = false;
  std::string error;
  int status = 0;
  std::string reason;
  std::string body;
  bool complete = false;
  std::string contentType;

  bool operator==(const Outcome& o) const {
    return failed == o.failed && status == o.status && reason == o.reason && body == o.body &&
           complete == o.complete && contentType == o.contentType;
  }
};

class Recorder : public Http1ResponseParser::Sink {
 public:
  void OnHead(const HttpResponseHead& head) override {
    outcome.status = head.status;
    outcome.reason = head.reason;
    if (auto type = head.Header("content-type")) outcome.contentType = std::string(*type);
  }
  void OnBody(std::span<const uint8_t> data) override { outcome.body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnComplete() override { outcome.complete = true; }
  Outcome outcome;
};

std::span<const uint8_t> Bytes(const std::string& s, size_t from, size_t to) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()) + from, to - from);
}

// Feeds `input` in the given pieces, then reports the end of the connection.
Outcome Run(const std::string& input, const std::vector<size_t>& cuts, bool headRequest = false) {
  Recorder recorder;
  Http1ResponseParser parser(recorder, headRequest);
  size_t from = 0;
  std::vector<size_t> ends = cuts;
  ends.push_back(input.size());
  for (size_t end : ends) {
    if (auto error = parser.Feed(Bytes(input, from, end))) {
      recorder.outcome.failed = true;
      recorder.outcome.error = *error;
      return recorder.outcome;
    }
    from = end;
  }
  if (auto error = parser.Finish()) {
    recorder.outcome.failed = true;
    recorder.outcome.error = *error;
  }
  return recorder.outcome;
}

int total = 0;
int failed = 0;

void Check(const char* name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name, detail.c_str());
}

// The result must not depend on where the bytes happen to be cut.
Outcome RunEverywhere(const char* name, const std::string& input, bool headRequest = false) {
  Outcome whole = Run(input, {}, headRequest);
  for (size_t cut = 1; cut < input.size(); ++cut) {
    Outcome split = Run(input, {cut}, headRequest);
    if (!(split == whole)) {
      Check(name, false, "differs when cut at byte " + std::to_string(cut));
      return whole;
    }
  }
  std::vector<size_t> everyByte;
  for (size_t i = 1; i < input.size(); ++i) everyByte.push_back(i);
  Check(name, Run(input, everyByte, headRequest) == whole, "differs when fed byte by byte");
  return whole;
}

void Accepts(const char* name, const std::string& input, int status, const std::string& body, bool headRequest = false) {
  Outcome o = RunEverywhere(name, input, headRequest);
  Check(name, !o.failed && o.complete && o.status == status && o.body == body,
        "failed=" + std::to_string(o.failed) + " error=" + o.error + " status=" + std::to_string(o.status) + " body=[" + o.body + "]");
}

void Rejects(const char* name, const std::string& input) {
  Outcome o = RunEverywhere(name, input);
  Check(name, o.failed, "was accepted, body=[" + o.body + "]");
}

}  // namespace

int main() {
  Accepts("content-length", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 200, "hello");
  Accepts("chunked with extension and trailer",
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-T: v\r\n\r\n", 200,
          "hello world");
  Accepts("chunked, uppercase hex", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n", 200, "0123456789");
  Accepts("until close", "HTTP/1.1 200 OK\r\n\r\nabc", 200, "abc");
  Accepts("no content", "HTTP/1.1 204 No Content\r\n\r\n", 204, "");
  Accepts("not modified ignores its length", "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", 304, "");
  Accepts("empty body", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, "");
  Accepts("interim response is skipped",
          "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", 200, "hi");
  Accepts("no reason phrase", "HTTP/1.1 200\r\nContent-Length: 1\r\n\r\nx", 200, "x");
  Accepts("HTTP/1.0", "HTTP/1.0 200 OK\r\n\r\nold", 200, "old");
  Accepts("equal repeated lengths", "HTTP/1.1 200 OK\r\nContent-Length: 2, 2\r\nContent-Length: 2\r\n\r\nhi", 200, "hi");
  Accepts("head request has no body", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 200, "", true);
  {
    Outcome o = Run("HTTP/1.1 200 OK\r\nContent-Type:\ttext/html \r\nContent-Length: 0\r\n\r\n", {});
    Check("header lookup trims and ignores case", o.contentType == "text/html", o.contentType);
  }

  Rejects("not HTTP", "SPDY/3 200 OK\r\n\r\n");
  Rejects("HTTP/2 status line", "HTTP/2 200 OK\r\n\r\n");
  Rejects("short status code", "HTTP/1.1 20 OK\r\nContent-Length: 0\r\n\r\n");
  Rejects("bare LF line endings", "HTTP/1.1 200 OK\nContent-Length: 0\n\nhi");
  Rejects("bare CR", "HTTP/1.1 200 OK\r\nX: a\rb\r\nContent-Length: 0\r\n\r\n");
  Rejects("space before the colon", "HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n");
  Rejects("folded header", "HTTP/1.1 200 OK\r\nX: a\r\n b\r\nContent-Length: 0\r\n\r\n");
  Rejects("NUL in a value", "HTTP/1.1 200 OK\r\nX: a\0b\r\nContent-Length: 0\r\n\r\n"s);
  Rejects("control character in a value", "HTTP/1.1 200 OK\r\nX: a\x01" "b\r\nContent-Length: 0\r\n\r\n");
  Rejects("header without a name", "HTTP/1.1 200 OK\r\n: v\r\n\r\n");
  Rejects("Content-Length and Transfer-Encoding",
          "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
  Rejects("Content-Lengths that disagree", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi!");
  Rejects("Content-Length list that disagrees", "HTTP/1.1 200 OK\r\nContent-Length: 2, 3\r\n\r\nhi!");
  Rejects("Content-Length not a number", "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello");
  Rejects("negative Content-Length", "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n");
  Rejects("Content-Length overflow", "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n");
  Rejects("gzip transfer coding", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
  Rejects("transfer coding after chunked", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n0\r\n\r\n");
  Rejects("transfer encoding without chunked", "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nx");
  Rejects("101 switching protocols", "HTTP/1.1 101 Switching Protocols\r\n\r\n");
  Rejects("bad chunk size", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nz\r\nx\r\n0\r\n\r\n");
  Rejects("chunk size too long", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n123456789abcdef01\r\n");
  Rejects("chunk without its CRLF", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n");
  Rejects("chunk size with bare LF", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n0\r\n\r\n");
  Rejects("bad trailer", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nno colon here\r\n\r\n");
  Rejects("truncated fixed body", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello");
  Rejects("truncated chunked body", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel");
  Rejects("closed inside the head", "HTTP/1.1 200 OK\r\nContent-Le");
  Rejects("closed with nothing", "");
  Rejects("data after a fixed body", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiHTTP/1.1 200 OK\r\n\r\n");
  Rejects("data after a chunked body", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nextra");
  Rejects("head too large", "HTTP/1.1 200 OK\r\nX: " + std::string(70 * 1024, 'a') + "\r\n\r\n");

  std::printf("http1 parser: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
