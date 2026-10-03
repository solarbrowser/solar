#include "solar/net/Hsts.h"

#include <algorithm>
#include <optional>

namespace solar::net {

namespace {

bool IsTokenChar(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

bool IsIpAddress(std::string_view host) {
  if (host.starts_with('[')) return true;
  return !host.empty() && std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

struct Policy {
  uint64_t maxAge = 0;
  bool includeSubdomains = false;
};

// The RFC 6797 grammar: directives between semicolons, each a token with an optional token or
// quoted-string value, spaces and tabs allowed around them. Anything else makes it all invalid.
std::optional<Policy> ParsePolicy(std::string_view text) {
  size_t i = 0;
  const auto skipSpace = [&] {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  };

  std::optional<uint64_t> maxAge;
  bool includeSubdomains = false;
  bool sawIncludeSubdomains = false;

  while (true) {
    skipSpace();
    if (i < text.size() && text[i] != ';') {
      const size_t nameStart = i;
      while (i < text.size() && IsTokenChar(text[i])) ++i;
      if (i == nameStart) return std::nullopt;
      std::string name(text.substr(nameStart, i - nameStart));
      for (char& c : name) c = Lower(c);

      std::optional<std::string> value;
      skipSpace();
      if (i < text.size() && text[i] == '=') {
        ++i;
        skipSpace();
        std::string v;
        if (i < text.size() && text[i] == '"') {
          ++i;
          while (true) {
            if (i >= text.size()) return std::nullopt;
            const char c = text[i++];
            if (c == '"') break;
            if (c == '\\') {
              if (i >= text.size()) return std::nullopt;
              v.push_back(text[i++]);
            } else {
              v.push_back(c);
            }
          }
        } else {
          const size_t valueStart = i;
          while (i < text.size() && IsTokenChar(text[i])) ++i;
          if (i == valueStart) return std::nullopt;
          v.assign(text.substr(valueStart, i - valueStart));
        }
        value = std::move(v);
        skipSpace();
      }

      if (name == "max-age") {
        if (maxAge || !value || value->empty()) return std::nullopt;
        uint64_t seconds = 0;
        for (char c : *value) {
          if (c < '0' || c > '9') return std::nullopt;
          // Past what a 31-bit count of seconds can hold it is as good as forever, and must not wrap.
          seconds = std::min<uint64_t>(seconds * 10 + static_cast<uint64_t>(c - '0'), 0x7fffffffu);
        }
        maxAge = seconds;
      } else if (name == "includesubdomains") {
        if (sawIncludeSubdomains) return std::nullopt;
        sawIncludeSubdomains = true;
        includeSubdomains = true;
      }
      // Other directives are for future use and are skipped.
    }
    if (i >= text.size()) break;
    if (text[i] != ';') return std::nullopt;
    ++i;
  }

  if (!maxAge) return std::nullopt;
  return Policy{*maxAge, includeSubdomains};
}

}  // namespace

HstsStore::HstsStore(Clock clock) : clock_(clock ? std::move(clock) : Clock([] { return std::chrono::system_clock::now(); })) {}

void HstsStore::Note(std::string_view host, std::string_view headerValue) {
  if (host.empty() || IsIpAddress(host)) return;
  const std::optional<Policy> policy = ParsePolicy(headerValue);
  if (!policy) return;

  if (policy->maxAge == 0) {
    if (auto it = entries_.find(host); it != entries_.end()) entries_.erase(it);
    return;
  }
  entries_[std::string(host)] = Entry{clock_() + std::chrono::seconds(policy->maxAge), policy->includeSubdomains};
}

bool HstsStore::Covers(std::string_view host) const {
  if (host.empty()) return false;
  const auto now = clock_();

  if (auto it = entries_.find(host); it != entries_.end() && it->second.expires > now) return true;
  // Each domain above the host, for an entry that takes its subdomains in.
  for (size_t dot = host.find('.'); dot != std::string_view::npos; dot = host.find('.', dot + 1)) {
    if (auto it = entries_.find(host.substr(dot + 1)); it != entries_.end() && it->second.includeSubdomains && it->second.expires > now) {
      return true;
    }
  }
  return false;
}

}  // namespace solar::net
