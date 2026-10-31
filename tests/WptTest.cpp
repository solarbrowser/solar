#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/resource.h>
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
#include "solar/web/Console.h"
#include "solar/web/Messaging.h"
#include "solar/web/DomBindings.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/web/FetchBindings.h"
#include "solar/web/FetchHost.h"
#include "solar/web/JsEventLoop.h"
#include "solar/web/TimerHost.h"
#include "solar/web/UrlBindings.h"
#ifdef SOLAR_WPT_CANVAS
#include "solar/paint/Canvas.h"
#include "solar/web-api/ImageLoad.h"
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

// What pages and frames get from the test run: realms of one isolate, and the files of the tests as the web.
class TestEnvironment : public solar::html::FrameEnvironment {
 public:
  TestEnvironment(qe::Isolate& isolate, solar::net::Loop& loop, solar::net::HttpClient& client, solar::web::JsEventLoop& events)
      : isolate_(isolate), loop_(loop), client_(client), events_(events) {}

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
#ifdef SOLAR_WPT_CANVAS
      // (The canvas and WebGL are Skia's and the machine's: this build of the runner has them, as the window has.)
      solar::webapi::InstallCanvasNatives(realm);
      solar::webapi::InstallWebGlNatives(realm);
      solar::webapi::InstallImageLoadNatives(realm);
#endif
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.pageUrl = *solar::url::Parse("http://web-platform.test:8000/fetch/api/");
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
    });
    bundles_.push_back(std::move(bundle));
    return &realm;
  }

  std::optional<std::string> Load(const std::string& url) override {
    const std::string origin = "http://web-platform.test:8000/";
    if (!url.starts_with(origin)) return std::nullopt;
    std::string path = url.substr(origin.size());
    const std::string rest = path.substr(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
    path = path.substr(0, path.find_first_of("?#"));
    std::string contents;
    if (!ReadFile("tests/wpt/" + path, contents)) return std::nullopt;
    if (path.find(".sub.") != std::string::npos) contents = Substitute(contents, rest);
    return contents;
  }

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
      else if (name == "host" || name == "location[hostname]" || name == "domains[]") value = "web-platform.test";
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

  void ScriptFailed(const std::string& message) override { std::printf("  FAIL script error: %s\n", message.c_str()); }

 private:
  qe::Isolate& isolate_;
  std::map<std::string, std::pair<std::string, std::shared_ptr<qe::Script>>> scripts_;
  solar::net::Loop& loop_;
  solar::net::HttpClient& client_;
  solar::web::JsEventLoop& events_;
  std::vector<std::unique_ptr<RealmBundle>> bundles_;
};

// Where a page is on the server it is tested on: its path below tests/wpt (the web-platform-tests submodule), or /solar/ for the pages of our own (tests/wpt-solar/pages).
std::string PageUrlPath(const std::string& path) {
  const std::string own = "tests/wpt-solar/pages/";
  if (path.starts_with(own)) return "solar/" + path.substr(own.size());
  return std::filesystem::relative(path, "tests/wpt").string();
}

bool RunFile(const std::string& path, const std::string& harness, const std::string& resources,
             const std::string& skipFile) {
  std::string source;
  if (!ReadFile(path, source)) {
    std::printf("FAIL cannot open %s\n", path.c_str());
    return false;
  }
  std::string name = std::filesystem::path(path).filename();

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
      if (ReadFile(wanted, script)) prelude += script + "\n";
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
  auto loop = solar::net::Loop::Create();
  solar::net::HttpClient client(*loop);
  // The tasks of the page are the program's: timers go into its queue, which its event loop runs.
  solar::web::TimerHost timers;
  isolate->SetTimerProvider(timers.Provider());
  solar::web::JsEventLoop events(*loop, {[&] { isolate->PerformMicrotaskCheckpoint(); }, [&] { timers.RunDue(); },
                                         [&] { return timers.NextDelayMs(); }});
  TestEnvironment environment(*isolate, *loop, client, events);
  solar::html::SetFrameEnvironment(&environment);
  qe::Realm& realm = *environment.CreateRealm();
  qe::DefineGlobalFunction(realm.GetContext(), "gc", CollectGarbage, 0);

  std::string skips = "globalThis.__skip = [";
  for (const std::string& prefix : SkipsFor(skipFile, name)) skips += JsString(prefix) + ",";
  skips += "];";

  std::printf("%s\n", name.c_str());
  const bool isHtml = name.ends_with(".html") || name.ends_with(".xhtml") || name.ends_with(".svg") || name.ends_with(".htm");
  // A .window.js is a script of a page, which has a window and a document as any page does.
  // (The CSS ones declare themselves for window and worker, and the window is what they get.)
  const bool isWindowScript = name.ends_with(".window.js") || (name.ends_with(".any.js") && path.find("/wpt/css/") != std::string::npos);
  solar::dom::Document* document = nullptr;
  if (isHtml || isWindowScript) {
    // A page: it has a window and a document of its own, which its scripts run in.
    solar::html::RunInRealm(realm, [&] {
      document = solar::dom::NewDocument(realm.GetContext(), true);
      document->url = "http://web-platform.test:8000/" + PageUrlPath(path);
      solar::html::InstallWindow(realm, document);
    });
  }
  qe::EvaluateResult result = realm.Evaluate(std::string(std::getenv("WPT_ALL") ? "globalThis.__allFailures = true;\n" : "") + skips + resources + harness, "harness.js");
  if (result.ok && !prelude.empty()) result = realm.Evaluate(prelude, "prelude.js");
  if (result.ok && isWindowScript) {
    const std::string address = "http://web-platform.test:8000/" + PageUrlPath(path);
    if (!solar::html::LoadPage(realm, document, "<!DOCTYPE html><html><head></head><body></body></html>", "text/html")) result.ok = false;
    if (result.ok) result = realm.Evaluate(source, name);
  } else if (result.ok && isHtml) {
    const std::string address = "http://web-platform.test:8000/" + PageUrlPath(path);
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
