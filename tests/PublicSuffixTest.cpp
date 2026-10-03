#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/PublicSuffix.h"

namespace {

using solar::url::PublicSuffix;
using solar::url::RegistrableDomain;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

// The host a URL would hold for a name, as the cookie code will have it; none if it is no host.
std::optional<std::string> HostOf(const std::string& name) {
  const auto url = solar::url::Parse("https://" + name + "/");
  if (!url || !url->host) return std::nullopt;
  return *url->host;
}

// One argument of checkPublicSuffix(...) in the list's own tests: null, or a string in quotes.
std::optional<std::string> Argument(const std::string& text) {
  if (text == "null") return std::nullopt;
  return text.substr(1, text.size() - 2);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "tests/data";

  // The Public Suffix List project's own test vectors for the registrable domain.
  {
    std::ifstream file(dir + "/test_psl.txt");
    Check("test_psl.txt opens", static_cast<bool>(file));
    std::string line;
    int vectors = 0;
    while (std::getline(file, line)) {
      const std::string prefix = "checkPublicSuffix(";
      if (!line.starts_with(prefix)) continue;
      const size_t comma = line.find(", ", prefix.size());
      const size_t end = line.rfind(");");
      const auto input = Argument(line.substr(prefix.size(), comma - prefix.size()));
      const auto expected = Argument(line.substr(comma + 2, end - comma - 2));

      std::optional<std::string> actual;
      if (input) {
        if (const auto host = HostOf(*input)) {
          const std::string_view registrable = RegistrableDomain(*host);
          if (!registrable.empty()) actual = std::string(registrable);
        }
      }
      // The list gives its expectations in Unicode; ours are hosts, in punycode.
      std::optional<std::string> want = expected ? HostOf(*expected) : std::nullopt;
      ++vectors;
      Check(line, actual == want, "got " + actual.value_or("null"));
    }
    Check("vectors were read", vectors > 50, std::to_string(vectors));
  }

  // The suffix itself, which the vectors do not ask for.
  Check("a top-level domain", PublicSuffix("com") == "com" && PublicSuffix("example.com") == "com");
  Check("two labels", PublicSuffix("a.b.example.co.uk") == "co.uk");
  Check("a private suffix", PublicSuffix("alice.github.io") == "github.io");
  Check("a name no rule covers has its last label", PublicSuffix("host.invalid-tld") == "invalid-tld" && PublicSuffix("localhost") == "localhost");
  Check("a wildcard rule", PublicSuffix("foo.ck") == "foo.ck" && PublicSuffix("bar.foo.ck") == "foo.ck");
  Check("an exception to it", PublicSuffix("www.ck") == "ck" && PublicSuffix("a.www.ck") == "ck");
  Check("an address has none", PublicSuffix("127.0.0.1").empty() && PublicSuffix("[::1]").empty() && PublicSuffix("").empty());
  Check("an empty label gives nothing", PublicSuffix(".com").empty() && PublicSuffix("a..com").empty() && PublicSuffix("example.com.").empty());
  Check("the view points into the host", [] {
    const std::string host = "www.example.co.uk";
    const std::string_view suffix = PublicSuffix(host);
    return suffix.data() == host.data() + 12;
  }());
  Check("the registrable domain", RegistrableDomain("a.b.example.co.uk") == "example.co.uk" && RegistrableDomain("example.com") == "example.com");
  Check("a suffix has none", RegistrableDomain("co.uk").empty() && RegistrableDomain("com").empty() && RegistrableDomain("foo.ck").empty());
  Check("an address has none either", RegistrableDomain("127.0.0.1").empty());
  Check("an internationalized suffix, in punycode", PublicSuffix("example.xn--p1ai") == "xn--p1ai");

  // Same-site, which cookies use.
  const auto sameSite = [](const char* a, const char* b) { return solar::url::IsSameSite(*solar::url::Parse(a), *solar::url::Parse(b)); };
  Check("subdomains of one registrable domain are same-site", sameSite("https://a.example.com/", "https://b.example.com/") && sameSite("https://example.com/", "https://www.example.com/x"));
  Check("different registrable domains are not", !sameSite("https://example.com/", "https://example.org/") && !sameSite("https://a.github.io/", "https://b.github.io/"));
  Check("under a public suffix, hosts are same-site only if the same", !sameSite("https://a.co.uk/", "https://b.co.uk/") && sameSite("https://a.co.uk/", "https://a.co.uk/"));
  Check("the scheme counts and the port does not", !sameSite("http://example.com/", "https://example.com/") && sameSite("https://example.com:8443/", "https://example.com/"));
  Check("addresses and localhost have no registrable domain", sameSite("http://127.0.0.1:1/", "http://127.0.0.1:2/") && !sameSite("http://127.0.0.1/", "http://127.0.0.2/") &&
                                                             sameSite("http://localhost/", "http://localhost:3/") && !sameSite("http://localhost/", "http://other.localhost/"));
  Check("an opaque origin is same-site with nothing", !sameSite("file:///a", "file:///a") && !sameSite("data:,x", "https://example.com/"));

  std::printf("public suffix: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
