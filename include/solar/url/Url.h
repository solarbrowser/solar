#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace solar::url {

struct Url {
  std::string scheme;
  std::string username;
  std::string password;
  // Serialized form: IPv6 keeps its brackets, an empty host is "" rather than nullopt.
  std::optional<std::string> host;
  std::optional<uint16_t> port;
  // Exactly one of these is in use: the spec models a path as either a list of
  // segments or a single opaque string, and opaquePath being set selects the latter.
  std::vector<std::string> path;
  std::optional<std::string> opaquePath;
  std::optional<std::string> query;
  std::optional<std::string> fragment;

  bool IsSpecial() const;
  bool IncludesCredentials() const { return !username.empty() || !password.empty(); }
};

bool IsSpecialScheme(std::string_view scheme);
std::optional<uint16_t> DefaultPort(std::string_view scheme);

bool IsWindowsDriveLetter(std::string_view s);
bool IsNormalizedWindowsDriveLetter(std::string_view s);
bool StartsWithWindowsDriveLetter(std::string_view s);

void ShortenPath(Url& url);

}  // namespace solar::url
