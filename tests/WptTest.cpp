#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "quanta/Embed.h"
#include "solar/web/UrlBindings.h"

namespace {

namespace qe = Quanta::Embed;

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

  auto runtime = qe::Runtime::Create();
  solar::web::InstallUrlApis(*runtime);

  std::string skips = "globalThis.__skip = [";
  for (const std::string& prefix : SkipsFor(skipFile, name)) skips += JsString(prefix) + ",";
  skips += "];";

  std::printf("%s\n", name.c_str());
  qe::Runtime::Result result = runtime->Evaluate(skips + resources + harness, "harness.js");
  if (result.ok) result = runtime->Evaluate(source, name);
  if (!result.ok) {
    std::printf("  FAIL while loading: %s\n", result.error.c_str());
    return false;
  }
  runtime->PerformMicrotaskCheckpoint();

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
