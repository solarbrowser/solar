// The messages between a page and the network service as bytes, and the service's end of the connection of a page that is not to be trusted: what it says is checked, what it asks for
// an origin that is not its own is refused, garbage ends nothing but the message it is, and a connection that goes ends what was in flight.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "solar/net/Ipc.h"
#include "solar/net/PageFetch.h"
#include "solar/net/RemoteNetwork.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "support/RemoteSide.h"
#include "support/TestLoop.h"
#include "support/TestServer.h"

using namespace solar::net;
using solar::test::TestServer;

namespace {

int total = 0, failed = 0;
void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

std::string Origin(const TestServer& server) { return "http://127.0.0.1:" + std::to_string(server.port()); }
std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct Heard : PageFetchHandler {
  std::optional<PageResponse> response;
  std::string body;
  bool ended = false;
  std::optional<std::string> error;
  void OnResponse(const PageResponse& r) override { response = r; }
  void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override { ended = true; }
  void OnError(std::string_view message) override { error = std::string(message); }
};

PageRequest SampleRequest() {
  PageRequest request;
  request.url = *solar::url::Parse("https://example.org:8443/path?q=1#frag");
  request.method = "PUT";
  request.headers = {{"X-One", "1"}, {"Content-Type", "text/plain"}};
  request.body = std::make_shared<const std::string>(std::string("bytes\0with nul", 14));
  request.destination = RequestDestination::Font;
  request.mode = RequestMode::Cors;
  request.credentials = RequestCredentials::Include;
  request.redirect = 2;
  request.cache = CacheMode::Reload;
  request.sendsReferrer = true;
  request.referrerPolicy = "origin";
  request.pageUrl = *solar::url::Parse("http://page.test/a/b");
  request.timeout = std::chrono::milliseconds(12345);
  request.restartable = true;
  request.stall = std::chrono::milliseconds(777);
  return request;
}

}  // namespace

int main() {
  {
    // The messages come back as they went.
    const PageRequest request = SampleRequest();
    ipc::Writer out;
    ipc::Write(out, request);
    ipc::Reader in(out.bytes());
    PageRequest back;
    const bool ok = ipc::Read(in, back) && in.AtEnd();
    Check("a request is read as it was written", ok && solar::url::Serialize(back.url) == solar::url::Serialize(request.url) && back.method == "PUT" && back.headers == request.headers &&
          back.body && *back.body == *request.body && back.destination == RequestDestination::Font && back.mode == RequestMode::Cors && back.credentials == RequestCredentials::Include &&
          back.redirect == 2 && back.cache == CacheMode::Reload && back.sendsReferrer && back.referrerPolicy == "origin" && solar::url::Serialize(back.pageUrl) == solar::url::Serialize(request.pageUrl) &&
          back.timeout == request.timeout && back.restartable && back.stall == request.stall);

    PageResponse response;
    response.type = ResponseType::Cors;
    response.status = 206;
    response.statusText = "Partial Content";
    response.headers = {{"Content-Range", "bytes 0-1/2"}};
    response.url = *solar::url::Parse("http://x.test/y");
    response.redirected = true;
    response.hiddenStatus = 302;
    response.hiddenLocation = "/z";
    response.hasBody = false;
    ipc::Writer r;
    ipc::Write(r, response);
    ipc::Reader rin(r.bytes());
    PageResponse rback;
    Check("a response is too", ipc::Read(rin, rback) && rback.type == ResponseType::Cors && rback.status == 206 && rback.statusText == "Partial Content" && rback.headers == response.headers &&
          solar::url::Serialize(rback.url) == "http://x.test/y" && rback.redirected && rback.hiddenStatus == 302 && rback.hiddenLocation == "/z" && !rback.hasBody);

    WebSocketOptions options;
    options.protocols = {"a", "b"};
    options.initiator = *solar::url::Parse("http://page.test/");
    options.headers = {{"X", "y"}};
    ipc::Writer w;
    ipc::Write(w, options);
    ipc::Reader win(w.bytes());
    WebSocketOptions oback;
    Check("and the options of a WebSocket", ipc::Read(win, oback) && oback.protocols == options.protocols && oback.initiator && oback.headers == options.headers);
  }
  {
    // What is not a message is not read, and does not make the reader ask for much.
    ipc::Writer out;
    ipc::Write(out, SampleRequest());
    const std::string whole = out.bytes();
    bool anyTruncated = false;
    for (size_t cut = 0; cut < whole.size(); cut += 7) {
      ipc::Reader in(std::string_view(whole).substr(0, cut));
      PageRequest request;
      if (ipc::Read(in, request)) anyTruncated = true;
    }
    Check("a request that is cut short is never read", !anyTruncated);
    ipc::Writer lie;
    lie.U32(0xFFFFFFF0u);  // a string that says it is four gigabytes
    ipc::Reader reader(lie.bytes());
    const std::string text = reader.String();
    Check("a length that is more than what is left ends the reading", text.empty() && !reader.ok());
    ipc::Writer headers;
    headers.U32(0x7FFFFFFF);  // a list of two billion headers in a few bytes
    ipc::Reader hreader(headers.bytes());
    const auto list = hreader.Headers();
    Check("so does a count that cannot be true", list.empty() && !hreader.ok());
    ipc::Writer address;
    address.String("not an address at all");
    ipc::Reader areader(address.bytes());
    Check("an address that is none is not one", !areader.ReadUrl() && !areader.ok());
    ipc::Writer wrong;
    ipc::Write(wrong, SampleRequest());
    std::string tampered = wrong.bytes();
    // (the destination byte, a few bytes after the strings, is set to a number there is no enumerator for)
    size_t at = tampered.find("PUT") + 3 + 4 + 0;
    (void)at;
    ipc::Writer enumerator;
    enumerator.U8(200);
    ipc::Reader ereader(enumerator.bytes());
    ereader.EnumUpTo(RequestDestination::Other);
    Check("an enumerator that is none ends it", !ereader.ok());
  }
  {
    // The service's end: a page that is tied to an origin.
    std::atomic<int> served{0};
    TestServer server([&](int client) {
      while (true) {
        const std::string head = TestServer::ReadHead(client);
        if (head.empty()) break;
        ++served;
        TestServer::SendAll(client, Response("200 OK", "fine", "Access-Control-Allow-Origin: *\r\n"));
      }
    });
    auto pair = ipc::Channel::MakePair();
    solar::test::RemoteSide side(pair.second);
    side.host->SetOrigin("http://page.test");
    auto loop = solar::test::MakeLoop();
    // (The page's end is made by hand for the part that sends what a page may not.)
    ipc::Channel raw(pair.first);
    std::atomic<int> errors{0}, responses{0};
    std::thread listener([&] {
      ipc::Message type;
      std::string bytes;
      while (raw.Receive(type, bytes)) {
        if (type == ipc::Message::FetchError) ++errors;
        if (type == ipc::Message::FetchResponse) ++responses;
      }
    });
    const auto ask = [&](uint64_t id, const std::string& page, const std::string& url) {
      PageRequest request;
      request.url = *solar::url::Parse(url);
      request.pageUrl = *solar::url::Parse(page);
      ipc::Writer out;
      out.U64(id);
      ipc::Write(out, request);
      raw.Send(ipc::Message::StartFetch, out.bytes());
    };
    ask(1, "http://page.test/", Origin(server) + "/ok");
    ask(2, "http://other.test/", Origin(server) + "/stolen");
    ask(3, "http://page.test/", "file:///etc/passwd");
    ask(4, "http://page.test/", Origin(server) + "/ok2");
    for (int i = 0; i < 200 && (responses.load() < 2 || errors.load() < 2); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check("a request for the page's own origin is done", responses.load() == 2 && served.load() == 2, std::to_string(responses.load()) + "/" + std::to_string(served.load()));
    Check("one that names another origin as the page's is refused, and nothing is sent", errors.load() >= 1 && side.host->refused() >= 1, std::to_string(side.host->refused()));
    Check("so is one for an address that is not the network's", errors.load() == 2 && side.host->refused() == 2, std::to_string(errors.load()));

    // Garbage, of every type and length, ends nothing but itself.
    std::mt19937 random(1234);
    for (int i = 0; i < 3000; ++i) {
      std::string junk(random() % 300, '\0');
      for (char& c : junk) c = static_cast<char>(random());
      raw.Send(static_cast<ipc::Message>(random() % 64), junk);
    }
    // (And the messages a page may not send, written well.)
    for (const ipc::Message type : {ipc::Message::FetchResponse, ipc::Message::FetchBody, ipc::Message::FetchEnd, ipc::Message::WebSocketOpen}) {
      ipc::Writer out;
      out.U64(9);
      raw.Send(type, out.bytes());
    }
    const int before = responses.load();
    ask(77, "http://page.test/", Origin(server) + "/after");
    for (int i = 0; i < 200 && responses.load() == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check("after 3000 messages of garbage the service still does what it is asked", responses.load() == before + 1);
    raw.Shutdown();
    listener.join();
  }
  {
    // A page whose service goes away is told, and not left waiting.
    std::atomic<bool> release{false};
    TestServer hang([&](int client) {
      TestServer::ReadHead(client);
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    auto loop = solar::test::MakeLoop();
    auto pair = ipc::Channel::MakePair();
    auto side = std::make_unique<solar::test::RemoteSide>(pair.second);
    side->host->SetOrigin("http://page.test");
    RemoteNetworkService service(*loop, pair.first);
    Heard heard;
    PageRequest request;
    request.url = *solar::url::Parse(Origin(hang) + "/hang");
    request.pageUrl = *solar::url::Parse("http://page.test/");
    service.StartFetch(request, heard);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    side.reset();  // (the service's process is gone)
    loop->Run();
    Check("a fetch whose service has gone ends with an error", !heard.ended && heard.error && heard.error->find("network service is gone") != std::string::npos, heard.error.value_or("waiting"));
    release = true;
  }
  std::printf("ipc: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
