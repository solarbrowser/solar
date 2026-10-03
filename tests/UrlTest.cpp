#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"
#include "solar/url/Utf8.h"
#include "support/Json.h"

namespace {

using solar::test::Json;
using solar::test::Load;

std::string Get(const solar::url::Url& url, const std::string& property) {
  using namespace solar::url;
  if (property == "href") return Serialize(url);
  if (property == "origin") return SerializeOrigin(url);
  if (property == "protocol") return GetProtocol(url);
  if (property == "username") return GetUsername(url);
  if (property == "password") return GetPassword(url);
  if (property == "host") return GetHost(url);
  if (property == "hostname") return GetHostname(url);
  if (property == "port") return GetPort(url);
  if (property == "pathname") return GetPathname(url);
  if (property == "search") return GetSearch(url);
  if (property == "hash") return GetHash(url);
  std::fprintf(stderr, "unknown property %s\n", property.c_str());
  std::exit(2);
}

void Report(const char* name, int total, int failed) {
  std::printf("%s: %d/%d passed\n", name, total - failed, total);
}

int RunUrlTests(const Json& root) {
  int total = 0;
  int failed = 0;
  for (const Json& test : root.array) {
    if (test.type != Json::Type::Object) continue;
    ++total;

    const std::string& input = test.Find("input")->string;
    const Json* baseJson = test.Find("base");
    bool hasBase = baseJson && baseJson->type == Json::Type::String;
    std::optional<solar::url::Url> base;
    if (hasBase) base = solar::url::Parse(baseJson->string);

    std::optional<solar::url::Url> url;
    if (!hasBase || base) url = solar::url::Parse(input, base ? &*base : nullptr);

    const Json* failure = test.Find("failure");
    std::string want = failure && failure->boolean ? "<failure>" : test.Find("href")->string;
    std::string got = url ? solar::url::Serialize(*url) : "<failure>";

    if (got == want && url) {
      for (const char* property : {"origin", "protocol", "username", "password", "host", "hostname", "port",
                                   "pathname", "search", "hash"}) {
        const Json* expected = test.Find(property);
        if (expected && Get(*url, property) != expected->string) {
          got = std::string(property) + "=" + Get(*url, property);
          want = std::string(property) + "=" + expected->string;
          break;
        }
      }
    }

    if (got != want) {
      ++failed;
      std::printf("FAIL input=%s base=%s\n  want %s\n  got  %s\n", input.c_str(),
                  hasBase ? baseJson->string.c_str() : "null", want.c_str(), got.c_str());
    }
  }
  Report("urltestdata", total, failed);
  return failed;
}

void Set(solar::url::Url& url, const std::string& property, const std::string& value) {
  using namespace solar::url;
  if (property == "href") {
    if (std::optional<Url> parsed = Parse(value)) url = *parsed;
  } else if (property == "protocol") {
    SetProtocol(url, value);
  } else if (property == "username") {
    SetUsername(url, value);
  } else if (property == "password") {
    SetPassword(url, value);
  } else if (property == "host") {
    SetHost(url, value);
  } else if (property == "hostname") {
    SetHostname(url, value);
  } else if (property == "port") {
    SetPort(url, value);
  } else if (property == "pathname") {
    SetPathname(url, value);
  } else if (property == "search") {
    SetSearch(url, value);
  } else if (property == "hash") {
    SetHash(url, value);
  } else {
    std::fprintf(stderr, "unknown property %s\n", property.c_str());
    std::exit(2);
  }
}

int RunSetterTests(const Json& root) {
  int total = 0;
  int failed = 0;
  for (const auto& [property, cases] : root.object) {
    if (property == "comment") continue;
    for (const Json& test : cases.array) {
      ++total;
      std::optional<solar::url::Url> url = solar::url::Parse(test.Find("href")->string);
      if (!url) {
        ++failed;
        std::printf("FAIL setter %s: cannot parse %s\n", property.c_str(), test.Find("href")->string.c_str());
        continue;
      }
      Set(*url, property, test.Find("new_value")->string);

      for (const auto& [name, expected] : test.Find("expected")->object) {
        std::string got = Get(*url, name);
        if (got == expected.string) continue;
        ++failed;
        std::printf("FAIL %s of %s = %s\n  %s: want %s\n  got  %s\n", property.c_str(),
                    test.Find("href")->string.c_str(), test.Find("new_value")->string.c_str(), name.c_str(),
                    expected.string.c_str(), got.c_str());
        break;
      }
    }
  }
  Report("setters", total, failed);
  return failed;
}

// toascii.json and IdnaTestV2.json are both exercised the way WPT does it: as the host of
// https://<input>/x, with a null output meaning the whole URL fails to parse.
int RunHostTests(const char* name, const Json& root) {
  int total = 0;
  int failed = 0;
  for (const Json& test : root.array) {
    if (test.type != Json::Type::Object) continue;

    const std::string& input = test.Find("input")->string;
    // WPT skips this too: https:///x is a different parse, not an empty host.
    if (input.empty()) continue;
    // Assigned in Unicode 18.0, while WPT's expectations predate it and still disallow them.
    bool newInUnicode18 = false;
    for (char32_t c : solar::url::DecodeUtf8(input)) newInUnicode18 = newInUnicode18 || (c >= 0x3D000 && c <= 0x3FC3F);
    if (newInUnicode18) continue;

    ++total;
    const Json* output = test.Find("output");
    std::string want = output->type == Json::Type::String ? "https://" + output->string + "/x" : "<failure>";

    std::optional<solar::url::Url> url = solar::url::Parse("https://" + input + "/x");
    std::string got = url ? solar::url::Serialize(*url) : "<failure>";

    if (got != want) {
      ++failed;
      std::printf("FAIL %s input=%s\n  want %s\n  got  %s\n", name, input.c_str(), want.c_str(), got.c_str());
    }
  }
  Report(name, total, failed);
  return failed;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "tests/data";
  int failed = 0;

  std::optional<Json> urls = Load(dir + "/urltestdata.json");
  std::optional<Json> toAscii = Load(dir + "/toascii.json");
  std::optional<Json> idna = Load(dir + "/IdnaTestV2.json");
  std::optional<Json> setters = Load(dir + "/setters_tests.json");
  if (!urls || !toAscii || !idna || !setters) return 2;

  failed += RunUrlTests(*urls);
  failed += RunSetterTests(*setters);
  failed += RunHostTests("toascii", *toAscii);
  failed += RunHostTests("IdnaTestV2", *idna);
  return failed == 0 ? 0 : 1;
}
