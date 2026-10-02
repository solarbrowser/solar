#include "solar/url/Url.h"

namespace solar::url {

namespace {

bool IsAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

}  // namespace

bool Url::IsSpecial() const { return IsSpecialScheme(scheme); }

bool IsSpecialScheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" ||
         scheme == "ftp" || scheme == "file";
}

std::optional<uint16_t> DefaultPort(std::string_view scheme) {
  if (scheme == "http" || scheme == "ws") return 80;
  if (scheme == "https" || scheme == "wss") return 443;
  if (scheme == "ftp") return 21;
  return std::nullopt;
}

bool IsWindowsDriveLetter(std::string_view s) {
  return s.size() == 2 && IsAsciiAlpha(s[0]) && (s[1] == ':' || s[1] == '|');
}

bool IsNormalizedWindowsDriveLetter(std::string_view s) {
  return IsWindowsDriveLetter(s) && s[1] == ':';
}

bool StartsWithWindowsDriveLetter(std::string_view s) {
  if (s.size() < 2 || !IsWindowsDriveLetter(s.substr(0, 2))) return false;
  return s.size() == 2 || s[2] == '/' || s[2] == '\\' || s[2] == '?' || s[2] == '#';
}

void ShortenPath(Url& url) {
  if (url.scheme == "file" && url.path.size() == 1 && IsNormalizedWindowsDriveLetter(url.path[0])) {
    return;
  }
  if (!url.path.empty()) url.path.pop_back();
}

}  // namespace solar::url
