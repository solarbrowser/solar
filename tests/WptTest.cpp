#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "quanta/Embed.h"
#include "solar/web/DomBindings.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/web/FetchBindings.h"
#include "solar/web/FetchHost.h"
#include "solar/web/JsEventLoop.h"
#include "solar/web/UrlBindings.h"

namespace {

namespace qe = Quanta::Embed;

// The runtime of the file being run, for the gc() that WPT's garbageCollect looks for.
qe::Runtime* g_runtime = nullptr;

Quanta::Value CollectGarbage(Quanta::Context&, Quanta::Value, qe::Args, Quanta::Value) {
  if (g_runtime) g_runtime->CollectGarbage();
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

  // Declared in the order they must be destroyed in, last first: the host goes before the client it
  // cancels through, and before the runtime whose realm it settles promises in.
  auto runtime = qe::Runtime::Create();
  g_runtime = runtime.get();
  qe::DefineGlobalFunction(runtime->GetContext(), "gc", CollectGarbage, 0);
  auto loop = solar::net::Loop::Create();
  solar::net::HttpClient client(*loop);
  solar::web::InstallUrlApis(*runtime);
  solar::web::InstallDomApis(*runtime);
  solar::web::InstallFetchApis(*runtime);
  solar::web::FetchHost::Config hostConfig;
  hostConfig.loop = loop.get();
  hostConfig.client = &client;
  hostConfig.pageUrl = *solar::url::Parse("http://web-platform.test:8000/fetch/api/");
  solar::web::JsEventLoop events(*loop, {[&] { runtime->PerformMicrotaskCheckpoint(); }, [&] { runtime->RunDueTimers(); },
                                         [&] { return runtime->NextTimerDelayMs(); }});
  hostConfig.afterScript = [&] { events.AfterScript(); };
  solar::web::FetchHost host(runtime->GetContext(), hostConfig);

  std::string skips = "globalThis.__skip = [";
  for (const std::string& prefix : SkipsFor(skipFile, name)) skips += JsString(prefix) + ",";
  skips += "];";

  std::printf("%s\n", name.c_str());
  qe::Runtime::Result result = runtime->Evaluate(skips + resources + harness, "harness.js");
  if (result.ok && !prelude.empty()) result = runtime->Evaluate(prelude, "prelude.js");
  if (result.ok) result = runtime->Evaluate(source, name);
  if (!result.ok) {
    std::printf("  FAIL while loading: %s\n", result.error.c_str());
    return false;
  }
  runtime->PerformMicrotaskCheckpoint();
  // Tests that wait for the network or for a timer: run both, up to a limit so that a test that never
  // ends does not hold the run.
  events.Run(std::chrono::seconds(10));

  result = runtime->Evaluate("__wptFinish()", name);
  return result.ok;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered, so that the last line before a crash is the file that crashed.
  std::setbuf(stdout, nullptr);
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <test.any.js>...\n", argv[0]);
    return 2;
  }
  std::string harness;
  if (!ReadFile("tests/wpt/harness.js", harness)) {
    std::fprintf(stderr, "cannot open tests/wpt/harness.js\n");
    return 2;
  }

  std::string resources = LoadResources("tests/data");

  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    if (!RunFile(argv[i], harness, resources, "tests/wpt/skip.tsv")) ++failed;
  }
  std::printf("wpt: %d of %d files passed\n", argc - 1 - failed, argc - 1);
  return failed == 0 ? 0 : 1;
}
