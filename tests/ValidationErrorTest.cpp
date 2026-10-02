#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "solar/url/Parser.h"

namespace {

using solar::url::ValidationError;
using solar::url::ValidationErrors;

int total = 0;
int failed = 0;

struct Case {
  std::string input;
  ValidationError error;
  bool parses;
  std::optional<std::string> base = std::nullopt;
};

std::string Names(const ValidationErrors& errors) {
  std::string out;
  for (ValidationError error : errors) out += std::string(out.empty() ? "" : ", ") + solar::url::ValidationErrorName(error);
  return out.empty() ? "none" : out;
}

void Run(const Case& test) {
  ++total;
  std::optional<solar::url::Url> base;
  if (test.base) base = solar::url::Parse(*test.base);

  ValidationErrors errors;
  std::optional<solar::url::Url> url = solar::url::Parse(test.input, base ? &*base : nullptr, &errors);

  bool reported = false;
  for (ValidationError error : errors) reported = reported || error == test.error;

  if (reported && url.has_value() == test.parses) return;
  ++failed;
  std::printf("FAIL %s: want %s and %s, got [%s] and %s\n", test.input.c_str(),
              solar::url::ValidationErrorName(test.error), test.parses ? "success" : "failure",
              Names(errors).c_str(), url ? "success" : "failure");
}

}  // namespace

// Every example in the standard's validation error table, with whether the table says the
// error makes the parse fail.
int main() {
  using E = ValidationError;
  const std::vector<Case> cases = {
      {"https://exa%23mple.org", E::DomainToAscii, false},
      {"https://exam%70le.org", E::DomainPercentEncoded, true},
      {"foo://exa[mple.org", E::HostInvalidCodePoint, false},
      {"https://127.0.0.1./", E::Ipv4EmptyPart, true},
      {"https://1.2.3/", E::Ipv4TooFewParts, true},
      {"https://1.2.3.4.5/", E::Ipv4TooManyParts, false},
      {"https://test.42", E::Ipv4NonNumericPart, false},
      {"https://127.0.0x0.1", E::Ipv4NonDecimalPart, true},
      {"https://255.255.4000.1", E::Ipv4OutOfRangePart, false},
      {"https://\xE2\x91\xA0.\xE2\x91\xA1.\xE2\x91\xA2.\xE2\x91\xA3", E::Ipv4NonAsciiInput, true},
      {"https://[::1", E::Ipv6Unclosed, false},
      {"https://[:1]", E::Ipv6InvalidCompression, false},
      {"https://[1:2:3:4:5:6:7:8:9]", E::Ipv6TooManyPieces, false},
      {"https://[1::1::1]", E::Ipv6MultipleCompression, false},
      {"https://[1:2:3!:4]", E::Ipv6InvalidCodePoint, false},
      {"https://[1:2:3:]", E::Ipv6InvalidCodePoint, false},
      {"https://[1:2:3]", E::Ipv6TooFewPieces, false},
      {"https://[::01]", E::Ipv6PieceLeadingZero, true},
      {"https://[1:1:1:1:1:1:1:127.0.0.1]", E::Ipv4InIpv6TooManyPieces, false},
      {"https://[ffff::.0.0.1]", E::Ipv4InIpv6InvalidCodePoint, false},
      {"https://[ffff::127.0.xyz.1]", E::Ipv4InIpv6InvalidCodePoint, false},
      {"https://[ffff::127.0xyz]", E::Ipv4InIpv6InvalidCodePoint, false},
      {"https://[ffff::127.00.0.1]", E::Ipv4InIpv6InvalidCodePoint, false},
      {"https://[ffff::127.0.0.1.2]", E::Ipv4InIpv6InvalidCodePoint, false},
      {"https://[ffff::127.0.0.4000]", E::Ipv4InIpv6OutOfRangePart, false},
      {"https://[ffff::127.0.0]", E::Ipv4InIpv6TooFewParts, false},
      {"https://example.org/>", E::InvalidUrlUnit, true},
      {" https://example.org ", E::InvalidUrlUnit, true},
      {"ht\ntps://example.org", E::InvalidUrlUnit, true},
      {"https://example.org/%s", E::InvalidUrlUnit, true},
      {"https://example.org/?%s", E::InvalidUrlUnit, true},
      {"https://example.org/#%s", E::InvalidUrlUnit, true},
      {"data:a b", E::InvalidUrlUnit, true},
      {"data:%zz", E::InvalidUrlUnit, true},
      {"foo://%zz", E::InvalidUrlUnit, true},
      {"foo://a\xC2\x80", E::InvalidUrlUnit, true},
      {"https://example.org/\xC2\x80", E::InvalidUrlUnit, true},
      {"file:c:/my-secret-folder", E::SpecialSchemeMissingFollowingSolidus, true},
      {"https:example.org", E::SpecialSchemeMissingFollowingSolidus, true},
      {"https:foo.html", E::SpecialSchemeMissingFollowingSolidus, true, "https://example.org/"},
      {"\xF0\x9F\x92\xA9", E::MissingSchemeNonRelativeUrl, false},
      {"\xF0\x9F\x92\xA9", E::MissingSchemeNonRelativeUrl, false, "mailto:user@example.org"},
      {"https://example.org\\path\\to\\file", E::InvalidReverseSolidus, true},
      {"https://user@example.org", E::InvalidCredentials, true},
      {"ssh://user@example.org", E::InvalidCredentials, true},
      {"https://#fragment", E::HostMissing, false},
      {"https://:443", E::HostMissing, false},
      {"https://user:pass@", E::HostMissing, false},
      {"https://example.org:70000", E::PortOutOfRange, false},
      {"https://example.org:7z", E::PortInvalid, false},
      // The standard's own example here, "/c:/path/to/file", takes the file slash state and
      // so never reaches the check; a relative path starting with a drive letter does.
      {"c|/path/to/file", E::FileInvalidWindowsDriveLetter, true, "file:///d:/"},
      {"file://c:", E::FileInvalidWindowsDriveLetterHost, true},
  };
  for (const Case& test : cases) Run(test);

  for (const char* clean : {"https://example.org/", "https://127.0.0.1:8080/a/b?q=1#f",
                            "foo://example.org/%41", "https://[2001:db8::1]/", "https://xn--n3h.example/"}) {
    ++total;
    ValidationErrors errors;
    solar::url::Parse(clean, nullptr, &errors);
    if (!errors.empty()) {
      ++failed;
      std::printf("FAIL %s: expected no errors, got [%s]\n", clean, Names(errors).c_str());
    }
  }

  std::printf("validation errors: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
