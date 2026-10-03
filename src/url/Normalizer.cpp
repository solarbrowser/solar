#include "solar/url/Normalizer.h"

#include <array>

namespace solar::url {

namespace {

// These never take an authority, so "scheme:rest" is unambiguous. Any other
// "word:digits" must be read as host:port, since "localhost:3000" is also a
// valid URL with the scheme "localhost".
constexpr std::array<std::string_view, 6> kSchemesWithoutAuthority = {
    "about", "data", "mailto", "javascript", "blob", "view-source"};

bool IsSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// An IPv6 address typed without brackets, which a URL needs because ':' would otherwise start
// a port. Two or more colons and nothing but hex digits, colons and dots (for a trailing IPv4
// part) cannot be anything else; one colon is a host and a port, as in "localhost:3000".
bool IsBareIpv6(std::string_view host) {
  size_t colons = 0;
  for (char c : host) {
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (c == ':') {
      ++colons;
    } else if (!hex && c != '.') {
      return false;
    }
  }
  return colons >= 2;
}

}  // namespace

std::string Normalize(std::string_view typed) {
  while (!typed.empty() && IsSpace(typed.front())) typed.remove_prefix(1);
  while (!typed.empty() && IsSpace(typed.back())) typed.remove_suffix(1);
  if (typed.empty()) return {};

  size_t hostEnd = typed.find_first_of("/?#");
  std::string_view host = typed.substr(0, hostEnd);
  if (IsBareIpv6(host)) {
    std::string_view rest = hostEnd == std::string_view::npos ? std::string_view() : typed.substr(hostEnd);
    return "https://[" + std::string(host) + "]" + std::string(rest);
  }

  size_t colon = typed.find(':');
  if (colon != std::string_view::npos) {
    std::string_view scheme = typed.substr(0, colon);
    if (typed.substr(colon + 1).starts_with("//")) return std::string(typed);
    for (std::string_view known : kSchemesWithoutAuthority) {
      if (scheme.size() == known.size()) {
        bool same = true;
        for (size_t i = 0; i < scheme.size(); ++i) {
          char c = scheme[i];
          same = same && (c >= 'A' && c <= 'Z' ? c + 0x20 : c) == known[i];
        }
        if (same) return std::string(typed);
      }
    }
  }

  if (typed.starts_with("//")) return "https:" + std::string(typed);
  return "https://" + std::string(typed);
}

}  // namespace solar::url
