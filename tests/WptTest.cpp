#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN ::popen
#define PCLOSE ::pclose
#endif

#include "quanta/Embed.h"
#include "solar/css/CssBindings.h"
#include "solar/dom/NodeBindings.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/font/Database.h"
#include "solar/html/Csp.h"
#include "solar/html/Errors.h"
#include "solar/html/Frames.h"
#include "solar/html/Modules.h"
#include "solar/html/Parser.h"
#include "solar/html/HtmlBindings.h"
#include "solar/web/BlobUrls.h"
#include "solar/web/Console.h"
#include "solar/web/Messaging.h"
#include "solar/web/DomBindings.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/FetchBindings.h"
#include "solar/web-api/CacheStore.h"
#include "solar/web-api/Compression.h"
#include "solar/web-api/Crypto.h"
#include "solar/web-api/IndexedDb.h"
#include "solar/web-api/WebSocket.h"
#include "solar/web-api/Worker.h"
#include "solar/web/FetchHost.h"
#include "solar/web/MimeType.h"
#include "solar/web/Orb.h"
#include "solar/web/JsEventLoop.h"
#include "solar/web/TimerHost.h"
#include "solar/web/UrlBindings.h"
#include "solar/net/Resolver.h"
#include "support/Pki.h"
#include "support/TestServer.h"
#include "support/TlsTestServer.h"
#ifdef SOLAR_WPT_CANVAS
#include "solar/paint/Canvas.h"
#include "solar/web-api/ImageLoad.h"
#include "solar/web-api/Media.h"
#endif

namespace {

namespace qe = Quanta::Embed;

// The isolate of the file being run, for the gc() that WPT's garbageCollect looks for.
qe::Isolate* g_isolate = nullptr;

Quanta::Value CollectGarbage(Quanta::Context&, Quanta::Value, qe::Args, Quanta::Value) {
  if (g_isolate) g_isolate->CollectGarbage();
  return qe::Undefined();
}

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream file(path);
  if (!file) return false;
  std::stringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// The JSON files as one script that defines globalThis.__resources. JSON text is valid
// JavaScript, so the data loads as literals and the runner needs no host function to read files.
std::string LoadResources(const std::string& dir) {
  std::string script = "globalThis.__resources = {";
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".json") continue;
    std::string contents;
    ReadFile(entry.path().string(), contents);
    script += "\"" + entry.path().filename().string() + "\": " + contents + ",\n";
  }
  return script + "};\n";
}

std::string JsString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out + "\"";
}

// Prefixes of the test names to skip in `file`, from skip.tsv.
std::vector<std::string> SkipsFor(const std::string& skipFile, const std::string& file) {
  std::vector<std::string> prefixes;
  std::ifstream in(skipFile);
  std::string line;
  while (std::getline(in, line)) {
    size_t first = line.find('\t');
    size_t second = line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    if (line.substr(0, first) == file) prefixes.push_back(line.substr(first + 1, second - first - 1));
  }
  return prefixes;
}

// The console of a test: what it prints goes where it always does, and a test that listens for log entries (the way
// WebDriver BiDi has them) is told of each, by the harness's __solarConsoleEntry.
class HarnessConsole : public solar::web::ConsoleSink {
 public:
  void Message(Quanta::Context& ctx, const solar::web::ConsoleMessage& message) override {
    solar::web::DefaultConsoleSink()->Message(ctx, message);
    Quanta::Value hook = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarConsoleEntry");
    if (!qe::IsCallable(hook)) return;
    Quanta::Value entry = qe::NewObject(ctx);
    qe::Set(ctx, entry, "type", qe::FromUtf8(ctx, "console"));
    qe::Set(ctx, entry, "method", qe::FromUtf8(ctx, message.method));
    const char* level = message.level == solar::web::ConsoleLevel::Error ? "error" : message.level == solar::web::ConsoleLevel::Warn ? "warn" : message.level == solar::web::ConsoleLevel::Debug ? "debug" : "info";
    qe::Set(ctx, entry, "level", qe::FromUtf8(ctx, level));
    qe::Set(ctx, entry, "text", qe::FromWtf8(ctx, message.text));
    Quanta::Value args = qe::NewArray(ctx, {});
    for (const Quanta::Value& arg : message.args) {
      Quanta::Value item = qe::NewObject(ctx);
      const char* type = arg.is_string() ? "string" : arg.is_number() ? "number" : arg.is_boolean() ? "boolean" : arg.is_undefined() ? "undefined" : arg.is_null() ? "null" : arg.is_symbol() ? "symbol" : arg.is_bigint() ? "bigint" : "object";
      qe::Set(ctx, item, "type", qe::FromUtf8(ctx, type));
      if (arg.is_string()) qe::Set(ctx, item, "value", arg);
      qe::ArrayPush(ctx, args, item);
    }
    qe::Set(ctx, entry, "args", args);
    qe::Call(ctx, hook, qe::Undefined(), qe::Args(&entry, 1));
    if (qe::HasException(ctx)) ctx.clear_exception();
  }
};

// A realm with every API of a window, and the host that fetch() in it needs. The host goes first, as it cancels what
// is in flight through the realm.
struct RealmBundle {
  std::unique_ptr<qe::Realm> realm;
  std::unique_ptr<solar::web::FetchHost> host;
  ~RealmBundle() {
    host.reset();
    solar::html::ForgetRealm(realm.get());
    realm.reset();
  }
};

// The port the test server listens on (set in main), for what the pages load from the names and ports it answers to.
int g_serverPort = 0;
std::shared_ptr<solar::net::TlsContext> g_tls;

// One request to the test server, waited for: the status, the Content-Type and the body of the answer. For the loader of the pages
// (a script from another origin, a frame with a pipe in its address), which asks the server as the network would.
struct ServerAnswer {
  bool ok = false;
  int status = 0;
  std::string contentType, body;
  bool noSniff = false;
};

ServerAnswer AskServer(const std::string& host, const std::string& target) {
  ServerAnswer answer;
#ifdef __linux__
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || g_serverPort == 0) return answer;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(g_serverPort));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) { ::close(fd); return answer; }
  const std::string request = "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  ::send(fd, request.data(), request.size(), 0);
  std::string raw;
  char buffer[65536];
  ssize_t got;
  while ((got = ::recv(fd, buffer, sizeof buffer, 0)) > 0) raw.append(buffer, static_cast<size_t>(got));
  ::close(fd);
  const size_t blank = raw.find("\r\n\r\n");
  if (blank == std::string::npos) return answer;
  answer.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
  std::istringstream lines(raw.substr(0, blank));
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string lower = line;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.starts_with("x-content-type-options:") && lower.find("nosniff") != std::string::npos) answer.noSniff = true;
    if (lower.starts_with("content-type:")) {
      answer.contentType = line.substr(13);
      while (!answer.contentType.empty() && answer.contentType.front() == ' ') answer.contentType.erase(answer.contentType.begin());
    }
  }
  answer.body = raw.substr(blank + 4);
  answer.ok = true;
#else
  (void)host;
  (void)target;
#endif
  return answer;
}

// What pages and frames get from the test run: realms of one isolate, and the files of the tests as the web.
// What a handler in Python (tools/wptpy.py) answered.
struct PyAnswer {
  bool ok = false;
  bool raw = false;  // body is the whole response, to be sent as it is
  int status = 500;
  std::string reason, contentType = "text/plain", headers, body;
};

// Runs the handler in `file` for a request: the request is written for it as a file, and what it prints is the answer.
PyAnswer RunPythonHandler(const std::string& file, const std::string& method, const std::string& url, const std::string& headerLines, const std::string& body) {
  static std::atomic<int> counter{0};
  PyAnswer result;
  const std::string requestFile = (std::filesystem::temp_directory_path() / ("wptpy-" + std::to_string(static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count())) + "-" + std::to_string(counter++) + ".req")).string();
  {
    std::ofstream out(requestFile, std::ios::binary);
    out << method << "\r\n" << url << "\r\n" << headerLines << "\r\n\r\n" << body;
  }
  const std::string command = "python3 tools/wptpy.py '" + file + "' '" + requestFile + "'" + (std::getenv("WPT_PY_DEBUG") ? "" : " 2>/dev/null");
  std::string answer;
  if (FILE* pipe = POPEN(command.c_str(), "r")) {
    char buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) answer.append(buffer, got);
    PCLOSE(pipe);
  }
  std::filesystem::remove(requestFile);
  if (answer.starts_with("RAW\n")) {
    result.ok = result.raw = true;
    result.body = answer.substr(4);
    return result;
  }
  const size_t statusEnd = answer.find('\n');
  const size_t blank = answer.find("\n\n");
  if (statusEnd == std::string::npos || blank == std::string::npos) return result;
  result.ok = true;
  result.status = std::atoi(answer.c_str());
  const size_t reasonAt = answer.find(' ');
  result.reason = answer.substr(reasonAt + 1, statusEnd - reasonAt - 1);
  std::istringstream lines(answer.substr(statusEnd + 1, blank - statusEnd));
  std::string headerLine;
  while (std::getline(lines, headerLine)) {
    if (headerLine.empty()) continue;
    const size_t colon = headerLine.find(':');
    if (colon == std::string::npos) continue;
    std::string name = headerLine.substr(0, colon), value = headerLine.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    std::string lowerName = name;
    for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowerName == "content-type") result.contentType = value;
    else if (lowerName == "content-length" || lowerName == "connection") continue;
    else result.headers += name + ": " + value + "\r\n";
  }
  result.body = answer.substr(blank + 2);
  return result;
}

class TestEnvironment : public solar::html::FrameEnvironment, public solar::webapi::WorkerHost {
 public:
  TestEnvironment(qe::Isolate& isolate, solar::net::Loop& loop, solar::net::HttpClient& client, solar::web::JsEventLoop& events)
      : isolate_(isolate), loop_(loop), client_(client), events_(events) {
    solar::webapi::SetWorkerHost(this);
  }
  ~TestEnvironment() override { solar::webapi::SetWorkerHost(nullptr); }

  // The workers of the pages: a realm each, of the same isolate, with the APIs a worker has.
  qe::Realm* CreateWorkerRealm(const std::string& scriptUrl) override {
    auto bundle = std::make_unique<RealmBundle>();
    qe::Isolate::RealmOptions options;
    options.installConsole = false;
    bundle->realm = isolate_.CreateRealm(options);
    qe::Realm& realm = *bundle->realm;
    solar::html::RunInRealm(realm, [&] {
      solar::web::InstallUrlApis(realm);
      static HarnessConsole console;
      solar::web::InstallConsoleApi(realm, &console);
      solar::web::InstallDomApis(realm);
      solar::web::InstallFetchApis(realm);
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.tls = g_tls;
      config.pageUrl = *solar::url::Parse(scriptUrl.rfind("blob:", 0) == 0 || scriptUrl.rfind("data:", 0) == 0 || scriptUrl.empty() ? (pageUrl_.empty() ? "about:blank" : pageUrl_) : scriptUrl);
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
      solar::webapi::InstallWebSocketNatives(realm);
      solar::webapi::InstallCryptoNatives(realm);
      solar::webapi::InstallCacheStoreNatives(realm);
      solar::webapi::InstallCompressionNatives(realm);
      solar::webapi::InstallWorkerScope(realm);
    });
    qe::Realm* raw = bundle->realm.get();
    bundles_.push_back(std::move(bundle));
    return raw;
  }
  void ReleaseWorkerRealm(qe::Realm* realm) override {
    for (auto it = bundles_.begin(); it != bundles_.end(); ++it) {
      if ((*it)->realm.get() == realm) {
        bundles_.erase(it);
        break;
      }
    }
  }
  void RunLater(std::function<void()> task) override { loop_.Post(std::move(task)); }
  std::optional<std::string> LoadScript(const std::string& url) override {
    if (url.starts_with("data:")) return solar::html::DecodeDataUrlText(url);
    if (url.starts_with("blob:")) {
      if (auto blob = solar::web::LookupBlobUrl(url)) return *blob->data;
      return std::nullopt;
    }
    return Load(url);
  }

  qe::Realm* CreateRealm() override {
    auto bundle = std::make_unique<RealmBundle>();
    // The console is Solar's own, which prints what is in an object and not "[object Object]".
    qe::Isolate::RealmOptions options;
    options.installConsole = false;
    bundle->realm = isolate_.CreateRealm(options);
    qe::Realm& realm = *bundle->realm;
    // The realm is set up as itself, which it has to be when another realm is the one running.
    solar::html::RunInRealm(realm, [&] {
      solar::web::InstallUrlApis(realm);
      static HarnessConsole console;
      solar::web::InstallConsoleApi(realm, &console);
      solar::web::InstallDomApis(realm);
      solar::dom::InstallNodeApis(realm);
      solar::html::InstallHtmlApis(realm);
      solar::css::InstallSelectorApis(realm);
      solar::web::InstallFetchApis(realm);
      solar::webapi::InstallWorkerApis(realm);
      solar::webapi::InstallIndexedDbNatives(realm);
      solar::webapi::InstallWebSocketNatives(realm);
      solar::webapi::InstallCryptoNatives(realm);
      solar::webapi::InstallCacheStoreNatives(realm);
      solar::webapi::InstallCompressionNatives(realm);
#ifdef SOLAR_WPT_CANVAS
      // (The canvas and WebGL are Skia's and the machine's: this build of the runner has them, as the window has.)
      solar::webapi::InstallCanvasNatives(realm);
      solar::webapi::InstallWebGlNatives(realm);
      solar::webapi::InstallImageLoadNatives(realm);
      solar::webapi::InstallMediaNatives(realm);
#endif
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.tls = g_tls;
      config.pageUrl = *solar::url::Parse(pageUrl_.empty() ? "http://web-platform.test:8000/fetch/api/" : pageUrl_);
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
    });
    bundles_.push_back(std::move(bundle));
    return &realm;
  }

  std::optional<std::string> Load(const std::string& url) override {
    const std::string origin = "http://web-platform.test:8000/";
    // Another origin of the test server (a name of its own, another port), or an address with a pipe, is asked of the server itself.
    if (const auto parsed = solar::url::Parse(url); parsed && (parsed->scheme == "http" || parsed->scheme == "https") && parsed->host) {
      const std::string host = *parsed->host;
      const bool served = host == "web-platform.test" || host.ends_with(".web-platform.test") || host == "not-web-platform.test" || host.ends_with(".not-web-platform.test");
      const bool secure = parsed->scheme == "https";
      const uint16_t port = parsed->port.value_or(secure ? 443 : 80);
      const bool piped = url.find("pipe=") != std::string::npos;
      const bool portServed = secure ? (port == 443 || (port >= 8443 && port <= 8450)) : (port == 80 || (port >= 8000 && port <= 8010));
      // (The server answers the same over TLS: what is read here is read from it without.)
      if (served && portServed && (secure || !url.starts_with(origin) || piped)) {
        std::string target = solar::url::SerializePath(*parsed);
        if (parsed->query) target += "?" + *parsed->query;
        const ServerAnswer answer = AskServer(host + ":" + std::to_string(port), target);
        if (!answer.ok || answer.status >= 400) return std::nullopt;
        serverTypes_[url] = answer.contentType;
        serverNoSniff_[url] = answer.noSniff;
        return answer.body;
      }
    }
    if (!url.starts_with(origin)) return std::nullopt;
    std::string path = url.substr(origin.size());
    const std::string rest = path.substr(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
    path = path.substr(0, path.find_first_of("?#"));
    std::string contents;
    if (path.ends_with(".py") && path.find("..") == std::string::npos && std::filesystem::is_regular_file("tests/wpt/" + path)) {
      const PyAnswer answer = RunPythonHandler("tests/wpt/" + path, "GET", url, "Host: web-platform.test:8000", "");
      if (!answer.ok) return std::nullopt;
      handlerTypes_[url] = answer.contentType;
      return answer.body;
    }
    if (!ReadFile("tests/wpt/" + path, contents)) return std::nullopt;
    if (path.find(".sub.") != std::string::npos) contents = Substitute(contents, rest);
    return contents;
  }

  // Opaque Response Blocking of what a page took with no CORS from another origin than its own: what it blocks has no text.
  std::optional<std::string> LoadNoCors(const std::string& url, bool styleSheet) override {
    blockedLoad_ = false;
    std::optional<std::string> text = Load(url);
    if (!text) return text;
    const auto parsed = solar::url::Parse(url);
    const auto page = solar::url::Parse(pageUrl_.empty() ? "http://web-platform.test:8000/" : pageUrl_);
    if (!parsed || !page || solar::url::SerializeOrigin(*parsed) == solar::url::SerializeOrigin(*page)) return text;
    const auto found = serverTypes_.find(url);
    if (found == serverTypes_.end()) return text;
    // (A style sheet of another origin is one only if it says it is.)
    if (styleSheet) {
      const auto mime = solar::web::ParseMimeType(found->second);
      if (!mime || mime->type != "text" || mime->subtype != "css") return blockedLoad_ = true, std::nullopt;
    }
    const auto nosniff = serverNoSniff_.find(url);
    const solar::web::OrbVerdict verdict = solar::web::OrbCheckHead(200, found->second, nosniff != serverNoSniff_.end() && nosniff->second, "");
    if (verdict == solar::web::OrbVerdict::Block || (verdict == solar::web::OrbVerdict::NeedBody && solar::web::OrbBodyIsBlocked(*text))) return blockedLoad_ = true, std::nullopt;
    return text;
  }

  // What a handler in Python said its answer is.
  std::string ContentType(const std::string& url) override {
    if (const auto served = serverTypes_.find(url); served != serverTypes_.end() && !served->second.empty()) return served->second;
    const auto found = handlerTypes_.find(url);
    if (found != handlerTypes_.end()) return found->second.substr(0, found->second.find(';')) == "text/html" || found->second.find("xml") != std::string::npos || found->second.find("svg") != std::string::npos ? found->second : "text/html";
    return FrameEnvironment::ContentType(url);
  }
  std::map<std::string, std::string> handlerTypes_, serverTypes_;
  std::map<std::string, bool> serverNoSniff_;
  // The address of the page being run, for what the realm's scripts resolve against.
  void SetPageUrl(const std::string& url) { pageUrl_ = url; }
  std::string pageUrl_;

  // What the WPT server does to a file with .sub. in its name: {{GET[name]}} is the parameter of the query, {{host}}, {{ports[http][0]}} and {{location[...]}} the server's.
  static std::string Substitute(const std::string& text, const std::string& rest) {
    std::map<std::string, std::string> query;
    if (rest.starts_with("?")) {
      const std::string q = rest.substr(1, rest.find('#') == std::string::npos ? std::string::npos : rest.find('#') - 1);
      size_t at = 0;
      while (at <= q.size()) {
        size_t end = q.find('&', at);
        if (end == std::string::npos) end = q.size();
        const std::string pair = q.substr(at, end - at);
        const size_t eq = pair.find('=');
        auto decode = [](const std::string& v) {
          std::string out;
          for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size() + 0 && std::isxdigit(static_cast<unsigned char>(v[i + 1])) && std::isxdigit(static_cast<unsigned char>(v[i + 2]))) { out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16)); i += 2; }
            else out += v[i] == '+' ? ' ' : v[i];
          }
          return out;
        };
        if (!pair.empty()) query[decode(eq == std::string::npos ? pair : pair.substr(0, eq))] = eq == std::string::npos ? "" : decode(pair.substr(eq + 1));
        at = end + 1;
      }
    }
    std::string out;
    size_t at = 0;
    while (true) {
      const size_t open = text.find("{{", at);
      if (open == std::string::npos) { out += text.substr(at); break; }
      const size_t close = text.find("}}", open);
      if (close == std::string::npos) { out += text.substr(at); break; }
      out += text.substr(at, open - at);
      const std::string name = text.substr(open + 2, close - open - 2);
      std::string value;
      bool known = true;
      if (name.starts_with("GET[") && name.ends_with("]")) { const auto found = query.find(name.substr(4, name.size() - 5)); value = found == query.end() ? "" : found->second; }
      else if (name.starts_with("domains[") && name.ends_with("]")) { const std::string label = name.substr(8, name.size() - 9); value = label.empty() ? "web-platform.test" : label + ".web-platform.test"; }
      else if (name == "host" || name == "location[hostname]") value = "web-platform.test";
      else if (name.starts_with("ports[https][") && name.size() == 15) value = std::to_string(8443 + (name[13] - '0'));
      else if (name == "ports[http][1]") value = "8001";  // (the server answers on any port a name is given: another port is another origin)
      else if (name.starts_with("ports[http][")) value = "8000";
      else if (name.starts_with("hosts[") && name.ends_with("]")) {
        // hosts[set][label]: a host of the set ("" the tests', alt the other one's), a subdomain of it for a label.
        const size_t close1 = name.find(']');
        const std::string set = name.substr(6, close1 - 6);
        std::string label = close1 + 2 < name.size() ? name.substr(close1 + 2, name.size() - close1 - 3) : "";
        value = (label.empty() ? "" : label + ".") + (set == "alt" ? "not-web-platform.test" : "web-platform.test");
      }
      else if (name == "location[host]") value = "web-platform.test:8000";
      else if (name == "location[port]" || name == "ports[http][0]") value = "8000";
      else if (name == "location[protocol]") value = "http:";
      else known = false;
      out += known ? value : text.substr(open, close + 2 - open);
      at = close + 2;
    }
    return out;
  }

  bool SkipScript(const std::string& url) override {
    // The harness is already in place; any other script is a file of the tests.
    const std::string path = url.substr(0, url.find_first_of("?#"));
    return path.ends_with("testharness.js") || path.ends_with("testharnessreport.js") || (path.find("testdriver") != std::string::npos && path.find("testdriver-actions") == std::string::npos);
  }

  // The scripts the pages load, parsed once for every realm that runs them. Keyed by address and text, so a changed
  // file is not stale.
  std::shared_ptr<qe::Script> CompileScript(const std::string& address, const std::string& code) override {
    auto found = scripts_.find(address);
    if (found != scripts_.end() && found->second.first == code) return found->second.second;
    qe::CompileResult result = isolate_.CompileScript(code, address);
    if (!result.script) return nullptr;  // the page compiles it, and reports the error as it does
    scripts_[address] = {code, result.script};
    return result.script;
  }

  void RunJobs() override { isolate_.PerformMicrotaskCheckpoint(); }

  // (A script that Opaque Response Blocking refused gets an error event, which is what the tests look for, and is no failure of the run.)
  void ScriptFailed(const std::string& message) override { if (!blockedLoad_) std::printf("  FAIL script error: %s\n", message.c_str()); }
  bool blockedLoad_ = false;

 private:
  qe::Isolate& isolate_;
  std::map<std::string, std::pair<std::string, std::shared_ptr<qe::Script>>> scripts_;
  solar::net::Loop& loop_;
  solar::net::HttpClient& client_;
  solar::web::JsEventLoop& events_;
  std::vector<std::unique_ptr<RealmBundle>> bundles_;
};

// Where a page is on the server it is tested on: its path below tests/wpt (the web-platform-tests submodule), or /solar/ for the pages of our own (tests/wpt-solar/pages).
// Where the pages of the run are served from: a page that says it is https is served as that.
std::string PageBase(const std::string& name) {
  return name.find(".https.") != std::string::npos ? "https://web-platform.test:8443/" : "http://web-platform.test:8000/";
}

std::string PageUrlPath(const std::string& path) {
  const std::string own = "tests/wpt-solar/pages/";
  if (path.starts_with(own)) return "solar/" + path.substr(own.size());
  return std::filesystem::relative(path, "tests/wpt").string();
}


// The server the pages are tested against (web-platform.test and its subdomains): it gives what is in tests/wpt (and /solar/ from tests/wpt-solar/pages), with the Content-Type the extension says, the
// headers of a file's .headers, and the substitutions of a .sub. file. WPT's own server has handlers in Python that are not here: what they answer is a 404.
std::string ServerContentType(const std::string& path) {
  static const std::map<std::string, std::string> types = {
      {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"}, {"xhtml", "application/xhtml+xml"}, {"xht", "application/xhtml+xml"}, {"svg", "image/svg+xml"},
      {"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"}, {"css", "text/css"}, {"json", "application/json"}, {"png", "image/png"}, {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"webp", "image/webp"}, {"avif", "image/avif"}, {"ico", "image/x-icon"}, {"bmp", "image/bmp"}, {"apng", "image/apng"},
      {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"ogv", "video/ogg"}, {"ogg", "audio/ogg"}, {"oga", "audio/ogg"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"txt", "text/plain"},
      {"xml", "application/xml"}, {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"}, {"otf", "font/otf"}, {"pdf", "application/pdf"}, {"vtt", "text/vtt"}, {"wasm", "application/wasm"}};
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const auto found = types.find(path.substr(dot + 1));
  return found == types.end() ? "application/octet-stream" : found->second;
}

std::string UrlDecode(const std::string& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '%' && i + 2 < v.size() + 0 && std::isxdigit(static_cast<unsigned char>(v[i + 1])) && std::isxdigit(static_cast<unsigned char>(v[i + 2]))) { out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16)); i += 2; }
    else out += v[i];
  }
  return out;
}

// How the server reads a request and answers it: over a socket, or over a TLS connection.
struct WptIo {
  std::function<std::string()> readHead;
  std::function<std::string(size_t)> readBytes;
  std::function<void(std::string_view)> sendAll;
  const char* scheme = "http";
};

void ServeWptIo(WptIo& io) {
  const std::string head = io.readHead();
  const size_t lineEnd = head.find("\r\n");
  const std::string line = head.substr(0, lineEnd);
  const size_t a = line.find(' '), b = line.find(' ', a + 1);
  if (a == std::string::npos || b == std::string::npos) return;
  const std::string method = line.substr(0, a);
  std::string target = line.substr(a + 1, b - a - 1);
  const size_t q = target.find('?');
  const std::string query = q == std::string::npos ? "" : target.substr(q);
  std::string path = UrlDecode(q == std::string::npos ? target : target.substr(0, q));
  path = path.substr(0, path.find('#'));
  auto reply = [&](int status, const std::string& reason, const std::string& type, const std::string& body, const std::string& extra, bool headOnly) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" + (type == "\x01none" ? std::string() : "Content-Type: " + type + "\r\n") + ((extra.rfind("Content-Length:", 0) == std::string::npos && extra.find("\r\nContent-Length:") == std::string::npos) ? "Content-Length: " + std::to_string(body.size()) + "\r\n" : std::string()) + "Connection: close\r\n" + extra + "\r\n";
    if (!headOnly) out += body;
    io.sendAll(out);
  };
  std::string file;
  const std::string own = "/solar/";
  if (path.starts_with(own)) file = "tests/wpt-solar/pages/" + path.substr(own.size());
  else file = "tests/wpt" + path;
  // A handler in Python (what WPT's own server runs): the request goes to tools/wptpy.py, which answers.
  if (file.ends_with(".py") && path.find("..") == std::string::npos && std::filesystem::is_regular_file(file)) {
    std::string lengthText;
    size_t length = 0;
    {
      std::string lower = head;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t at = lower.find("\r\ncontent-length:");
      if (at != std::string::npos) length = static_cast<size_t>(std::strtoull(head.c_str() + at + 17, nullptr, 10));
    }
    const std::string body = length ? io.readBytes(length) : std::string();
    std::string host = "web-platform.test";
    {
      std::string lower = head;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t at = lower.find("\r\nhost:");
      if (at != std::string::npos) {
        const size_t end = head.find("\r\n", at + 2);
        host = head.substr(at + 7, end - at - 7);
        while (!host.empty() && host.front() == ' ') host.erase(host.begin());
      }
    }
    const PyAnswer answer = RunPythonHandler(file, method, std::string(io.scheme) + "://" + host + target, head.substr(lineEnd + 2, head.size() >= lineEnd + 6 ? head.size() - lineEnd - 6 : 0), body);
    if (!answer.ok) { reply(500, "Internal Server Error", "text/plain", "handler failed", "", method == "HEAD"); return; }
    if (answer.raw) { io.sendAll(answer.body); return; }
    std::string headers = answer.headers;
    if (headers.find("Access-Control-Allow-Origin") == std::string::npos && headers.find("access-control-allow-origin") == std::string::npos) headers += "Access-Control-Allow-Origin: *\r\n";
    reply(answer.status, answer.reason, answer.contentType, answer.body, headers, method == "HEAD");
    return;
  }
  if (method != "GET" && method != "HEAD") { reply(405, "Method Not Allowed", "text/plain", "", "", true); return; }
  if (path.find("..") != std::string::npos || !std::filesystem::is_regular_file(file)) { if (std::getenv("WPT_PY_DEBUG")) std::fprintf(stderr, "404 %s %s\n", method.c_str(), path.c_str()); reply(404, "Not Found", "text/plain", "not found", "", method == "HEAD"); return; }
  std::string contents;
  ReadFile(file, contents);
  if (file.find(".sub.") != std::string::npos) contents = TestEnvironment::Substitute(contents, query);
  // (The files are readable from any origin: WPT's own server adds the headers its tests ask for in the address, which this one does not parse.)
  std::string type = ServerContentType(file), extra = "Access-Control-Allow-Origin: *\r\n";
  std::string sidecar;
  if (ReadFile(file + ".headers", sidecar)) {
    std::istringstream lines(sidecar);
    std::string headerLine;
    while (std::getline(lines, headerLine)) {
      if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
      const size_t colon = headerLine.find(':');
      if (colon == std::string::npos) continue;
      std::string name = headerLine.substr(0, colon), value = headerLine.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      if (name == "Content-Type" || name == "content-type") type = value;
      else extra += name + ": " + value + "\r\n";
    }
  }
  // (A sidecar that says who may read the file says it instead of the default.)
  if (extra.find("Access-Control-Allow-Origin: *\r\n") != extra.rfind("Access-Control-Allow-Origin: *\r\n") ||
      extra.find("Access-Control-Allow-Origin:") != extra.rfind("Access-Control-Allow-Origin:")) {
    extra.erase(extra.find("Access-Control-Allow-Origin: *\r\n"), std::string("Access-Control-Allow-Origin: *\r\n").size());
  }
  // The pipes of the address (?pipe=header(name,value)|status(code)|slice(start,end)), the part of wptserve's that the tests use.
  int status = 200;
  std::string reason = "OK";
  if (const size_t pipeAt = query.find("pipe="); pipeAt != std::string::npos) {
    std::string pipes = UrlDecode(query.substr(pipeAt + 5, query.find('&', pipeAt) == std::string::npos ? std::string::npos : query.find('&', pipeAt) - pipeAt - 5));
    // (Pipes that follow each other with no | between them are taken apart, and the arguments are trimmed.)
    for (size_t i = 0; i + 1 < pipes.size(); ++i) {
      if (pipes[i] == ')' && std::isalpha(static_cast<unsigned char>(pipes[i + 1]))) pipes.insert(i + 1, "|");
    }
    size_t from = 0;
    while (from <= pipes.size()) {
      size_t bar = pipes.find('|', from);
      if (bar == std::string::npos) bar = pipes.size();
      const std::string step = pipes.substr(from, bar - from);
      from = bar + 1;
      const size_t open = step.find('(');
      if (open == std::string::npos || step.back() != ')') continue;
      const std::string name = step.substr(0, open);
      std::vector<std::string> args;
      std::string current;
      for (size_t i = open + 1; i + 1 < step.size(); ++i) {
        if (step[i] == '\\' && i + 2 < step.size() && step[i + 1] == ',') { current.push_back(','); ++i; }
        else if (step[i] == ',') { args.push_back(current); current.clear(); }
        else current.push_back(step[i]);
      }
      args.push_back(current);
      for (std::string& arg : args) {
        while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
      }
      if (name == "status" && !args.empty()) {
        status = std::atoi(args[0].c_str());
        reason = args.size() > 1 ? args[1] : (status == 206 ? "Partial Content" : status == 404 ? "Not Found" : status == 302 ? "Found" : status == 204 ? "No Content" : "OK");
      } else if (name == "header" && args.size() >= 2) {
        std::string lowerName = args[0];
        for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerName == "content-type") { type = args[1].empty() ? "\x01none" : args[1]; continue; }
        // (the header is replaced; one with no value is taken away)
        std::string kept;
        std::istringstream lines(extra);
        std::string line;
        while (std::getline(lines, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          std::string lowerLine = line.substr(0, line.find(':'));
          for (char& c : lowerLine) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (lowerLine != lowerName) kept += line + "\r\n";
        }
        extra = kept;
        if (!args[1].empty()) extra += args[0] + ": " + args[1] + "\r\n";
      } else if (name == "slice" && args.size() >= 2) {
        const size_t start = args[0] == "null" ? 0 : static_cast<size_t>(std::atoll(args[0].c_str()));
        const size_t end = args[1] == "null" ? contents.size() : std::min<size_t>(contents.size(), static_cast<size_t>(std::atoll(args[1].c_str())));
        contents = start < end ? contents.substr(start, end - start) : std::string();
      }
    }
  }
  reply(status, reason, type, contents, extra, method == "HEAD");
}

void ServeWpt(int client) {
  using solar::test::TestServer;
  WptIo io;
  io.readHead = [client] { return TestServer::ReadHead(client); };
  io.readBytes = [client](size_t n) { return TestServer::ReadBytes(client, n); };
  io.sendAll = [client](std::string_view data) { TestServer::SendAll(client, data); };
  ServeWptIo(io);
}

// Names of the test's hosts go to the server on the loopback interface; the port the page says is the server's.
solar::net::SystemResolverOptions WptResolverOptions(uint16_t port, uint16_t tlsPort) {
  solar::net::SystemResolverOptions options;
  options.lookup = [port, tlsPort](const std::string& host, uint16_t requested, solar::net::SocketAddress::Family family) {
    solar::net::ResolveResult result;
    // (The ports of the test server, 8000 and the next ones, are all this one; any other, port 1 say, is nothing listening.)
    const bool secure = requested == 443 || (requested >= 8443 && requested <= 8450);
    const bool served = requested == 0 || requested == 80 || secure || (requested >= 8000 && requested <= 8010);
    // (The names that exist: the domain and its few subdomains; any other, nonexistent.web-platform.test say, is nothing.)
    const auto exists = [&](const std::string& domain) {
      if (host == domain) return true;
      for (const char* label : {"www", "www1", "www2", "xn--lve-6lad", "xn--n8j6ds53lwwkrqhv28a"}) {
        if (host == std::string(label) + "." + domain) return true;
      }
      return false;
    };
    if (served && (exists("web-platform.test") || exists("not-web-platform.test"))) {
      if (family == solar::net::SocketAddress::Family::IPv4) {
        solar::net::SocketAddress address;
        address.family = family;
        address.bytes = {127, 0, 0, 1};
        address.port = secure ? tlsPort : port;
        result.addresses.push_back(address);
      }
    } else {
      result.error = "cannot resolve " + host;
    }
    return result;
  };
  return options;
}

bool RunFile(const std::string& path, const std::string& harness, const std::string& resources,
             const std::string& skipFile) {
  std::string source;
  if (!ReadFile(path, source)) {
    std::printf("FAIL cannot open %s\n", path.c_str());
    return false;
  }
  std::string name = std::filesystem::path(path).filename();
  if (name.find(".sub.") != std::string::npos) source = TestEnvironment::Substitute(source, "");

  // "// META: script=file" asks for another file to run first, relative to this one.
  std::string prelude;
  {
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line) && line.starts_with("//")) {
      const std::string tag = "// META: script=";
      if (!line.starts_with(tag)) continue;
      std::string script;
      // A path from the root is from tests/wpt; any other is from the test's own directory.
      const std::string given = line.substr(tag.size());
      const std::string wanted = given.starts_with("/") ? "tests/wpt" + given : (std::filesystem::path(path).parent_path() / given).string();
      if (ReadFile(wanted, script)) {
        if (wanted.find(".sub.") != std::string::npos) script = TestEnvironment::Substitute(script, "");
        prelude += script + "\n";
      }
    }
  }

  // Declared in the order they must be destroyed in, last first: the realms go before the event loop and the client
  // they settle promises and cancel requests through, and before the isolate they live in.
  auto isolate = qe::Isolate::Create();
  g_isolate = isolate.get();
  isolate->SetModuleHooks(solar::html::MakeModuleHooks());
  isolate->SetCodeGenerationHooks(solar::html::csp::MakeCodeGenerationHooks());
  isolate->SetSerializationHooks(solar::web::MakeSerializationHooks());
  isolate->SetUncaughtExceptionHandler(solar::html::MakeUncaughtExceptionHandler());
  isolate->SetPromiseRejectionHandler(solar::html::MakeRejectionHandler());
  // Where an error came from, down to the line and column of each call in its stack.
  isolate->SetSourcePositionTracking(true);
  solar::test::TestServer server(ServeWpt);
  g_serverPort = server.port();
  // What the pages keep (databases, service worker registrations) is the run's own, and goes with it.
  if (!std::getenv("SOLAR_IDB_DIR")) {
    const std::filesystem::path keep = std::filesystem::temp_directory_path() / ("wpt-keep-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(keep);
    ::setenv("SOLAR_IDB_DIR", keep.c_str(), 1);
    static std::filesystem::path toRemove;
    toRemove = keep;
    std::atexit([] { std::error_code ignored; std::filesystem::remove_all(toRemove, ignored); std::filesystem::remove_all(toRemove.string() + "-cache", ignored); });
  }
  // The same server over TLS, for the pages that ask for https: a private authority signs for the names of the tests, and the client trusts it.
  solar::test::TestPki pki;
  solar::test::TlsTestServer tlsServer(pki.Issue({"web-platform.test", "*.web-platform.test", "not-web-platform.test", "*.not-web-platform.test", "127.0.0.1", "localhost"}),
                                       [](ssl_st* connection) {
                                         WptIo io;
                                         io.scheme = "https";
                                         io.readHead = [connection] { return solar::test::TlsTestServer::ReadHead(connection); };
                                         io.readBytes = [connection](size_t n) { return solar::test::TlsTestServer::ReadBytes(connection, n); };
                                         io.sendAll = [connection](std::string_view data) { solar::test::TlsTestServer::SendAll(connection, data); };
                                         ServeWptIo(io);
                                       });
  solar::net::TlsOptions tlsOptions;
  tlsOptions.extraRootsPem = pki.rootPem();
  g_tls = solar::net::TlsContext::Create(tlsOptions);
  auto loop = solar::net::Loop::Create();
  solar::net::HttpClientOptions clientOptions;
  clientOptions.resolver = solar::net::MakeSystemResolver(*loop, WptResolverOptions(server.port(), tlsServer.port()));
  solar::net::HttpClient client(*loop, std::move(clientOptions));
  // The tasks of the page are the program's: timers go into its queue, which its event loop runs.
  solar::web::TimerHost timers;
  isolate->SetTimerProvider(timers.Provider());
  solar::web::JsEventLoop events(*loop, {[&] { isolate->PerformMicrotaskCheckpoint(); }, [&] { timers.RunDue(); },
                                         [&] { return timers.NextDelayMs(); }});
  TestEnvironment environment(*isolate, *loop, client, events);
  solar::html::SetFrameEnvironment(&environment);
  environment.SetPageUrl(PageBase(name) + PageUrlPath(path));
  qe::Realm& realm = *environment.CreateRealm();
  qe::DefineGlobalFunction(realm.GetContext(), "gc", CollectGarbage, 0);

  std::string skips = "globalThis.__skip = [";
  for (const std::string& prefix : SkipsFor(skipFile, name)) skips += JsString(prefix) + ",";
  skips += "];";

  std::printf("%s\n", name.c_str());
  const bool isHtml = name.ends_with(".html") || name.ends_with(".xhtml") || name.ends_with(".svg") || name.ends_with(".htm");
  // A .window.js is a script of a page, which has a window and a document as any page does.
  // (The CSS ones declare themselves for window and worker, and the window is what they get.)
  const bool isWindowScript = name.ends_with(".window.js") || (name.ends_with(".any.js") && (path.find("/wpt/css/") != std::string::npos || path.find("/wpt/xhr/") != std::string::npos || path.find("/wpt/fetch/") != std::string::npos || path.find("/wpt/websockets/") != std::string::npos));
  solar::dom::Document* document = nullptr;
  if (isHtml || isWindowScript) {
    // A page: it has a window and a document of its own, which its scripts run in.
    solar::html::RunInRealm(realm, [&] {
      document = solar::dom::NewDocument(realm.GetContext(), true);
      document->url = PageBase(name) + PageUrlPath(path);
      solar::html::InstallWindow(realm, document);
    });
  }
  qe::EvaluateResult result = realm.Evaluate(std::string(std::getenv("WPT_ALL") ? "globalThis.__allFailures = true;\n" : "") + skips + resources + harness, "harness.js");
  // A script that is not of a page (.any.js) has an address all the same, which is where its tests are served from.
  if (result.ok && !isHtml && !isWindowScript) {
    result = realm.Evaluate("Object.defineProperty(globalThis, 'location', { value: new URL(" + JsString(PageBase(name) + PageUrlPath(path)) + "), writable: true, configurable: true });", "location.js");
  }
  if (result.ok && !prelude.empty()) result = realm.Evaluate(prelude, "prelude.js");
  if (result.ok && isWindowScript) {
    const std::string address = PageBase(name) + PageUrlPath(path);
    if (!solar::html::LoadPage(realm, document, "<!DOCTYPE html><html><head></head><body></body></html>", "text/html")) result.ok = false;
    if (result.ok) result = realm.Evaluate(source, name);
  } else if (result.ok && isHtml) {
    const std::string address = PageBase(name) + PageUrlPath(path);
    if (!solar::html::LoadPage(realm, document, source, environment.ContentType(address))) {
      result.ok = false;
      result.error = "a script of the page failed";
    }
  } else if (result.ok) {
    result = realm.Evaluate(source, name);
  }
  if (!result.ok) {
    std::printf("  FAIL while loading: %s\n", result.error.c_str());
    solar::html::SetFrameEnvironment(nullptr);
    return false;
  }
  isolate->PerformMicrotaskCheckpoint();
  // Tests that wait for the network or for a timer: run both, up to a limit so that a test that never
  // ends does not hold the run.
  events.Run(std::chrono::seconds(10));

  result = realm.Evaluate("__wptFinish()", name);
  solar::html::SetFrameEnvironment(nullptr);
  return result.ok;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered, so that the last line before a crash is the file that crashed.
  std::setbuf(stdout, nullptr);
#ifdef __linux__
  // A test that runs away should fail on its own, not take the machine's memory with it.
  const char* gigabytes = std::getenv("WPT_LIMIT_GB");
  const unsigned long long bytes = (gigabytes ? std::strtoull(gigabytes, nullptr, 10) : 4ull) << 30;
  const rlimit limit = {bytes, bytes};
  setrlimit(RLIMIT_AS, &limit);
#endif
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <test.any.js>...\n", argv[0]);
    return 2;
  }
  std::string harness;
  if (!ReadFile("tests/wpt-solar/harness.js", harness)) {
    std::fprintf(stderr, "cannot open tests/wpt-solar/harness.js\n");
    return 2;
  }

  // Ahem, the font the layout tests are written for, is installed as if the machine had it.
  solar::font::Database::System().AddDirectory("tests/wpt-solar/fonts");

  std::string resources = LoadResources("tests/data");

  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    if (!RunFile(argv[i], harness, resources, "tests/wpt-solar/skip.tsv")) ++failed;
  }
  std::printf("wpt: %d of %d files passed\n", argc - 1 - failed, argc - 1);
  return failed == 0 ? 0 : 1;
}
