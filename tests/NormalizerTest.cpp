#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "solar/url/Normalizer.h"

int main() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"example.com", "https://example.com"},
      {"  example.com  ", "https://example.com"},
      {"", ""},
      {"//a.com/x", "https://a.com/x"},
      {"http://a.com/", "http://a.com/"},
      {"mailto:a@b.c", "mailto:a@b.c"},
      {"data:text/plain,hi", "data:text/plain,hi"},
      {"about:blank", "about:blank"},

      // One colon is a host and a port, even when everything around it looks like hex.
      {"localhost:3000", "https://localhost:3000"},
      {"127.0.0.1:8080", "https://127.0.0.1:8080"},
      {"dead:80", "https://dead:80"},

      // Two or more colons and only hex digits, colons and dots is an IPv6 address.
      {"2001:db8::1", "https://[2001:db8::1]"},
      {"2001:0db8:85a3::8a2e:0370:7334", "https://[2001:0db8:85a3::8a2e:0370:7334]"},
      {"::1", "https://[::1]"},
      {"::ffff:1.2.3.4", "https://[::ffff:1.2.3.4]"},
      {"2001:db8::1/path?q=1#f", "https://[2001:db8::1]/path?q=1#f"},
      {"2001:db8::1?q=1", "https://[2001:db8::1]?q=1"},

      // Already bracketed, so left to the URL parser.
      {"[::1]:8080/x", "https://[::1]:8080/x"},
      {"http://[::1]/", "http://[::1]/"},
  };

  int failed = 0;
  for (const auto& [input, want] : cases) {
    std::string got = solar::url::Normalize(input);
    if (got == want) continue;
    ++failed;
    std::printf("FAIL \"%s\": want \"%s\", got \"%s\"\n", input.c_str(), want.c_str(), got.c_str());
  }
  std::printf("normalizer: %zu/%zu passed\n", cases.size() - failed, cases.size());
  return failed == 0 ? 0 : 1;
}
