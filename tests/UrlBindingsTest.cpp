#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "quanta/core/gc/Collector.h"
#include "solar/web/UrlBindings.h"

namespace {

using Quanta::Embed::Runtime;

// Overwrites the stack below the caller. The collector scans conservatively, so a stale copy
// of a pointer the test has dropped would keep its cell alive and hide the bug being probed.
__attribute__((noinline)) void ScrubStack() {
  volatile char buffer[32768];
  for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = 0;
}

bool Run(Runtime& runtime, const std::string& source, const char* name) {
  Runtime::Result result = runtime.Evaluate(source, name);
  if (!result.ok) std::printf("FAIL %s: %s\n", name, result.error.c_str());
  return result.ok;
}

// A URL's query object is a cell of its own, referenced only from the URL's C++ member. Once
// the URL has survived a collection, the next minor one traces it only if it was told about
// the new edge, so this fails without the NoteWrite in the searchParams getter.
bool QueryObjectSurvivesMinorCollection(Runtime& runtime) {
  if (!Run(runtime, "globalThis.keep = new URL('https://gc.example/?k=v');", "gc setup")) return false;
  ScrubStack();
  runtime.CollectGarbage();
  if (!Run(runtime, "keep.searchParams.marker = 'alive';", "gc create")) return false;
  ScrubStack();
  Quanta::Collector::collect_minor();
  return Run(runtime,
             "if (keep.searchParams.marker !== 'alive' || keep.searchParams.get('k') !== 'v')"
             "  throw new Error('the query object was collected');",
             "gc check");
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "tests/js/UrlBindings.js";
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }
  std::stringstream source;
  source << file.rdbuf();

  auto runtime = Runtime::Create();
  solar::web::InstallUrlApis(*runtime);

  bool ok = Run(*runtime, source.str(), path);
  ok = QueryObjectSurvivesMinorCollection(*runtime) && ok;
  return ok ? 0 : 1;
}
