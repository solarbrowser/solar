#include "solar/url/PublicSuffix.h"

#include <algorithm>

#include "solar/url/Host.h"
#include "solar/url/PublicSuffixTables.h"

namespace solar::url {

namespace {

uint8_t FlagsOf(std::string_view key) {
  const psl::Rule* end = psl::kRules + psl::kRuleCount;
  const psl::Rule* it = std::lower_bound(psl::kRules, end, key, [](const psl::Rule& rule, std::string_view k) { return std::string_view(rule.key) < k; });
  return it != end && std::string_view(it->key) == key ? it->flags : 0;
}

size_t LabelCount(std::string_view name) { return static_cast<size_t>(std::count(name.begin(), name.end(), '.')) + 1; }

}  // namespace

// The algorithm of publicsuffix.org/list: of the rules that match, an exception rule wins, and
// otherwise the one with the most labels. A name no rule matches has its last label as its suffix.
std::string_view PublicSuffix(std::string_view host) {
  // A name with an empty label (".com", "a..b") is no name the list speaks of, though a URL's host may hold one.
  if (host.empty() || IsIpAddressHost(host) || host.starts_with('.') || host.ends_with('.') || host.find("..") != std::string_view::npos) return {};

  std::string_view best = host.substr(host.rfind('.') == std::string_view::npos ? 0 : host.rfind('.') + 1);
  size_t bestLabels = 1;

  // Each name from the whole host down to its last label, the host's suffixes in label steps.
  for (size_t start = 0;;) {
    const std::string_view name = host.substr(start);
    const size_t dot = name.find('.');
    const std::string_view parent = dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1);
    const uint8_t flags = FlagsOf(name);

    if (flags & psl::kException) return parent;  // "!www.ck": www.ck is registrable, its suffix is ck
    if ((flags & psl::kRule) && LabelCount(name) > bestLabels) {
      best = name;
      bestLabels = LabelCount(name);
    }
    if (!parent.empty() && (FlagsOf(parent) & psl::kWildcard) && LabelCount(name) > bestLabels) {
      best = name;
      bestLabels = LabelCount(name);
    }
    if (dot == std::string_view::npos) break;
    start += dot + 1;
  }
  return best;
}

std::string_view RegistrableDomain(std::string_view host) {
  const std::string_view suffix = PublicSuffix(host);
  if (suffix.empty() || suffix.size() == host.size()) return {};
  // The label before the suffix: back over the dot that joins them, then to the dot before that.
  const size_t labelEnd = host.size() - suffix.size() - 1;
  const size_t labelStart = host.rfind('.', labelEnd - 1);
  return host.substr(labelStart == std::string_view::npos ? 0 : labelStart + 1);
}

}  // namespace solar::url
