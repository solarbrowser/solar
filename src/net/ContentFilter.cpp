/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/ContentFilter.h"

#include <algorithm>
#include <cctype>

namespace solar::net {

namespace {

constexpr uint64_t kHostSeed = 0x9e3779b97f4a7c15ull;
constexpr uint64_t kTokenSeed = 0xcbf29ce484222325ull;

uint64_t Hash(std::string_view text, uint64_t seed) {
  uint64_t h = seed;
  for (unsigned char c : text) { h ^= c; h *= 0x100000001b3ull; }
  return h;
}

bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
// What `^` matches: any character but a letter, a digit, and _ - . %
bool IsSeparator(char c) { return !IsAlnum(c) && !(c >= 'A' && c <= 'Z') && c != '_' && c != '-' && c != '.' && c != '%'; }
bool IsHostChar(char c) { return IsAlnum(c) || c == '.' || c == '-'; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool HostIsOrUnder(std::string_view host, const std::string& domain) {
  if (host.size() < domain.size()) return false;
  if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) return false;
  return host.size() == domain.size() || host[host.size() - domain.size() - 1] == '.';
}

// Whether the segment (no `*` in it) matches at `pos`; where it does, `end` is where it ends.
bool MatchAt(std::string_view seg, const std::string& u, size_t pos, size_t& end) {
  for (char c : seg) {
    if (c == '^') {
      if (pos == u.size()) continue;  // the end of the address is a separator, and takes no character
      if (!IsSeparator(u[pos])) return false;
      ++pos;
    } else {
      if (pos >= u.size() || u[pos] != c) return false;
      ++pos;
    }
  }
  end = pos;
  return true;
}

// The segments of a pattern, matched one after the other: the first at `start` (or, where `floating`, anywhere from there), each next one at its first place after the one before.
bool MatchSegments(const std::string& pattern, const std::string& u, size_t start, bool floating, bool anchorEnd) {
  size_t pos = start;
  size_t seg_begin = 0;
  bool first = true;
  while (true) {
    size_t star = pattern.find('*', seg_begin);
    const bool last = star == std::string::npos;
    std::string_view seg(pattern.data() + seg_begin, (last ? pattern.size() : star) - seg_begin);
    if (!seg.empty() || (last && anchorEnd)) {
      size_t end = pos;
      bool found = false;
      const bool mustEnd = last && anchorEnd;
      if (first && !floating) {
        found = MatchAt(seg, u, pos, end) && (!mustEnd || end == u.size());
      } else {
        for (size_t p = pos; p <= u.size(); ++p) {
          if (MatchAt(seg, u, p, end) && (!mustEnd || end == u.size())) { found = true; break; }
        }
      }
      if (!found) return false;
      pos = end;
    }
    if (last) return true;
    seg_begin = star + 1;
    first = false;
  }
}

}  // namespace

void ContentFilter::AddList(std::string_view text) {
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    if (eol == std::string_view::npos) eol = text.size();
    ParseLine(Trim(text.substr(at, eol - at)));
    at = eol + 1;
  }
}

bool ContentFilter::ParseLine(std::string_view line) {
  if (line.empty() || line[0] == '!' || line[0] == '[') return false;
  for (const char* cosmetic : {"##", "#@#", "#?#", "#$#", "#%#", "#@$#", "#@?#"}) {
    if (line.find(cosmetic) != std::string_view::npos) return false;
  }
  Rule rule;
  if (line.substr(0, 2) == "@@") { rule.exception = true; line.remove_prefix(2); }

  std::string_view options;
  const size_t dollar = line.rfind('$');
  if (dollar != std::string_view::npos) {
    std::string_view tail = line.substr(dollar + 1);
    // An option list has none of the characters of a pattern (a `$` inside a pattern is rare, and left as it is).
    if (tail.find_first_of("*^/") == std::string_view::npos) { options = tail; line = line.substr(0, dollar); }
  }
  if (line.size() >= 2 && line.front() == '/' && line.back() == '/') return false;  // a regular expression

  if (!options.empty()) {
    uint16_t include = 0, exclude = 0;
    size_t at = 0;
    while (at <= options.size()) {
      size_t comma = options.find(',', at);
      if (comma == std::string_view::npos) comma = options.size();
      std::string_view option = options.substr(at, comma - at);
      at = comma + 1;
      if (option.empty()) continue;
      bool negated = false;
      if (option[0] == '~') { negated = true; option.remove_prefix(1); }
      std::string_view value;
      const size_t equals = option.find('=');
      if (equals != std::string_view::npos) { value = option.substr(equals + 1); option = option.substr(0, equals); }
      const std::string name = Lower(option);
      uint16_t type = 0;
      if (name == "script") type = kFilterScript;
      else if (name == "image") type = kFilterImage;
      else if (name == "stylesheet" || name == "css") type = kFilterStyle;
      else if (name == "xmlhttprequest" || name == "xhr") type = kFilterXhr;
      else if (name == "media") type = kFilterMedia;
      else if (name == "font") type = kFilterFont;
      else if (name == "subdocument" || name == "frame") type = kFilterSubdocument;
      else if (name == "websocket") type = kFilterWebSocket;
      else if (name == "ping") type = kFilterPing;
      else if (name == "other" || name == "object" || name == "object-subrequest") type = kFilterOther;
      if (type) { (negated ? exclude : include) |= type; continue; }
      if (name == "third-party" || name == "3p") { rule.party = negated ? 2 : 1; continue; }
      if (name == "first-party" || name == "1p") { rule.party = negated ? 1 : 2; continue; }
      if (name == "important" || name == "match-case" || name == "collapse" || name == "all") continue;
      if (name == "domain" && !value.empty()) {
        DomainSet set;
        size_t from = 0;
        while (from <= value.size()) {
          size_t bar = value.find('|', from);
          if (bar == std::string_view::npos) bar = value.size();
          std::string_view domain = value.substr(from, bar - from);
          from = bar + 1;
          if (domain.empty()) continue;
          if (domain[0] == '~') set.exclude.push_back(Lower(domain.substr(1)));
          else set.include.push_back(Lower(domain));
        }
        domains_.push_back(std::move(set));
        rule.domains = static_cast<uint32_t>(domains_.size());
        continue;
      }
      return false;  // popup, document, csp, redirect, removeparam, badfilter, denyallow...: not understood, so the rule is not used
    }
    if (include) rule.types = include;
    if (exclude) rule.types = static_cast<uint16_t>((include ? include : kFilterAllTypes) & ~exclude);
    if (!rule.types) return false;
  }

  if (line.substr(0, 2) == "||") { rule.anchorHost = true; line.remove_prefix(2); }
  else if (!line.empty() && line[0] == '|') { rule.anchorStart = true; line.remove_prefix(1); }
  if (!line.empty() && line.back() == '|') { rule.anchorEnd = true; line.remove_suffix(1); }
  while (!line.empty() && line.front() == '*' && !rule.anchorHost && !rule.anchorStart) line.remove_prefix(1);
  while (!line.empty() && line.back() == '*' && !rule.anchorEnd) line.remove_suffix(1);
  if (line.empty()) return false;
  if (line.find_first_of(" \t") != std::string_view::npos) return false;
  rule.pattern = Lower(line);

  // The key the rule is found by: the host a `||` rule names, or the longest word of the pattern that cannot be a part of a longer one in an address.
  uint64_t key = 0;
  bool keyed = false;
  const std::string& p = rule.pattern;
  if (rule.anchorHost) {
    size_t n = 0;
    while (n < p.size() && IsHostChar(p[n])) ++n;
    if (n > 0 && (n == p.size() || p[n] == '^' || p[n] == '/' || p[n] == ':' || p[n] == '?')) {
      key = Hash(std::string_view(p.data(), n), kHostSeed);
      keyed = true;
    }
  }
  if (!keyed) {
    size_t best = 0, best_at = 0;
    for (size_t i = 0; i < p.size();) {
      if (!IsAlnum(p[i])) { ++i; continue; }
      size_t j = i;
      while (j < p.size() && IsAlnum(p[j])) ++j;
      const bool leftOk = i > 0 ? p[i - 1] != '*' : (rule.anchorHost || rule.anchorStart);
      const bool rightOk = j < p.size() ? p[j] != '*' : rule.anchorEnd;
      if (leftOk && rightOk && j - i > best) { best = j - i; best_at = i; }
      i = j;
    }
    if (best >= 2) { key = Hash(std::string_view(p.data() + best_at, best), kTokenSeed); keyed = true; }
  }
  rules_.push_back(std::move(rule));
  const uint32_t id = static_cast<uint32_t>(rules_.size() - 1);
  if (keyed) index_.emplace_back(key, id);
  else unindexed_.push_back(id);
  return true;
}

void ContentFilter::Finish() {
  std::sort(index_.begin(), index_.end());
  index_.shrink_to_fit();
  rules_.shrink_to_fit();
}

bool ContentFilter::Matches(const Rule& rule, const FilterQuery& query, const std::string& u) const {
  if (!(rule.types & query.type)) return false;
  if (rule.party == 1 && !query.thirdParty) return false;
  if (rule.party == 2 && query.thirdParty) return false;
  if (rule.domains) {
    const DomainSet& set = domains_[rule.domains - 1];
    for (const std::string& d : set.exclude) if (HostIsOrUnder(query.pageHost, d)) return false;
    if (!set.include.empty()) {
      bool in = false;
      for (const std::string& d : set.include) in = in || HostIsOrUnder(query.pageHost, d);
      if (!in) return false;
    }
  }
  if (rule.anchorHost) {
    size_t begin = u.find("://");
    if (begin == std::string::npos) return false;
    begin += 3;
    size_t end = u.find_first_of("/?#", begin);
    if (end == std::string::npos) end = u.size();
    const size_t at = u.find('@', begin);
    if (at != std::string::npos && at < end) begin = at + 1;
    for (size_t p = begin; p < end; ++p) {
      if (p != begin && u[p - 1] != '.') continue;
      if (MatchSegments(rule.pattern, u, p, false, rule.anchorEnd)) return true;
    }
    return false;
  }
  if (rule.anchorStart) return MatchSegments(rule.pattern, u, 0, false, rule.anchorEnd);
  return MatchSegments(rule.pattern, u, 0, true, rule.anchorEnd);
}

bool ContentFilter::Blocks(const FilterQuery& query) const {
  if (rules_.empty()) return false;
  const std::string u = Lower(query.url);
  bool hit = false;
  auto consider = [&](uint32_t id) {
    const Rule& rule = rules_[id];
    if (rule.exception ? false : hit) return false;  // (a block that is already found needs only the exceptions)
    if (!Matches(rule, query, u)) return false;
    if (rule.exception) return true;
    hit = true;
    return false;
  };
  auto probe = [&](uint64_t key) {
    auto it = std::lower_bound(index_.begin(), index_.end(), std::make_pair(key, uint32_t{0}));
    for (; it != index_.end() && it->first == key; ++it) if (consider(it->second)) return true;
    return false;
  };
  // The host and every domain it is under.
  const std::string host = Lower(query.host);
  for (size_t at = 0; at < host.size();) {
    if (probe(Hash(std::string_view(host).substr(at), kHostSeed))) return false;
    const size_t dot = host.find('.', at);
    if (dot == std::string::npos) break;
    at = dot + 1;
  }
  // The words of the address.
  for (size_t i = 0; i < u.size();) {
    if (!IsAlnum(u[i])) { ++i; continue; }
    size_t j = i;
    while (j < u.size() && IsAlnum(u[j])) ++j;
    if (probe(Hash(std::string_view(u).substr(i, j - i), kTokenSeed))) return false;
    i = j;
  }
  for (uint32_t id : unindexed_) if (consider(id)) return false;
  return hit;
}

}  // namespace solar::net

// ---- The cosmetic rules ----

namespace solar::net {

namespace {

bool IsSimpleName(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!(IsAlnum(c) || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool Procedural(std::string_view s) {
  for (const char* marker : {":has-text(", ":-abp-", ":xpath(", ":matches-css", ":upward(", ":style(", ":remove(", ":contains(", ":nth-ancestor(", ":min-text-length(", ":matches-attr(",
                             ":matches-path(", ":matches-media(", ":watch-attr(", ":others(", ":not(:", "+js(", "{"}) {
    if (s.find(marker) != std::string_view::npos) return true;
  }
  return !s.empty() && s[0] == '^';
}

// Whether the end of the selector (the compound after the last combinator) has a class or an id: those are found by them, and the others would be tried for every element.
bool EndsWithClassOrId(std::string_view s) {
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '[' || c == '(') ++depth;
    else if (c == ']' || c == ')') --depth;
    else if (depth == 0 && (c == ' ' || c == '>' || c == '+' || c == '~' || c == ',')) start = i + 1;
  }
  depth = 0;
  for (size_t i = start; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '[' || c == '(') ++depth;
    else if (c == ']' || c == ')') --depth;
    else if (depth == 0 && (c == '.' || c == '#')) return true;
  }
  return false;
}

}  // namespace

void CosmeticFilter::AddList(std::string_view text) {
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    if (eol == std::string_view::npos) eol = text.size();
    ParseLine(Trim(text.substr(at, eol - at)));
    at = eol + 1;
  }
}

void CosmeticFilter::ParseLine(std::string_view line) {
  if (line.empty() || line[0] == '!' || line[0] == '[') return;
  // The mark that ends the domains: ## (hide) or #@# (an exception). The others (#?# #$# #%# #@?# #@$#) are for rules of more than a selector.
  size_t mark = std::string_view::npos;
  bool exception = false;
  for (size_t i = line.find('#'); i != std::string_view::npos; i = line.find('#', i + 1)) {
    if (line.compare(i, 2, "##") == 0) { mark = i; break; }
    if (line.compare(i, 3, "#@#") == 0) { mark = i; exception = true; break; }
    if (line.compare(i, 3, "#?#") == 0 || line.compare(i, 3, "#$#") == 0 || line.compare(i, 3, "#%#") == 0 || line.compare(i, 4, "#@$#") == 0 || line.compare(i, 4, "#@?#") == 0) return;
  }
  if (mark == std::string_view::npos) return;
  std::string_view domainText = line.substr(0, mark);
  std::string_view selector = Trim(line.substr(mark + (exception ? 3 : 2)));
  if (selector.empty() || Procedural(selector)) return;
  if (domainText.find_first_of(" \t/") != std::string_view::npos) return;  // (a network rule that has a ## in its address)

  std::vector<std::string> include, exclude;
  size_t from = 0;
  while (from <= domainText.size() && !domainText.empty()) {
    size_t comma = domainText.find(',', from);
    if (comma == std::string_view::npos) comma = domainText.size();
    std::string_view domain = domainText.substr(from, comma - from);
    from = comma + 1;
    if (domain.empty()) continue;
    if (domain[0] == '~') exclude.push_back(Lower(domain.substr(1)));
    else include.push_back(Lower(domain));
  }
  const std::string text(selector);
  if (exception) {
    exceptions_.push_back({text, std::move(include)});
    return;
  }
  if (include.empty()) {
    if (!exclude.empty()) return;
    if (selector[0] == '#' && IsSimpleName(selector.substr(1))) ids_.insert(text.substr(1));
    else if (selector[0] == '.' && IsSimpleName(selector.substr(1))) classes_.insert(text.substr(1));
    else if (EndsWithClassOrId(selector)) genericSelectors_.push_back(text);
    return;
  }
  for (const std::string& domain : include) byDomain_[domain].push_back({text, exclude});
  ++specificCount_;
}

void CosmeticFilter::Finish() {
  // An exception for every host takes the rule away.
  std::unordered_set<std::string> everywhere;
  for (const Exception& e : exceptions_) if (e.domains.empty()) everywhere.insert(e.selector);
  for (const std::string& selector : everywhere) {
    if (selector[0] == '#') ids_.erase(selector.substr(1));
    else if (selector[0] == '.') classes_.erase(selector.substr(1));
  }
  genericSelectors_.erase(std::remove_if(genericSelectors_.begin(), genericSelectors_.end(), [&](const std::string& s) { return everywhere.count(s) > 0; }), genericSelectors_.end());
}

std::shared_ptr<const css::HideRules> CosmeticFilter::ForHost(const std::string& rawHost) const {
  const std::string host = Lower(rawHost);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = cache_.find(host);
    if (found != cache_.end()) return found->second;
  }
  auto rules = std::make_shared<css::HideRules>();
  rules->ids = &ids_;
  rules->classes = &classes_;
  rules->selectors = genericSelectors_;
  std::vector<std::string> excepted;
  for (const Exception& e : exceptions_) {
    for (const std::string& d : e.domains) {
      if (HostIsOrUnder(host, d)) { excepted.push_back(e.selector); break; }
    }
  }
  for (size_t at = 0; at < host.size();) {
    const auto found = byDomain_.find(host.substr(at));
    if (found != byDomain_.end()) {
      for (const Specific& rule : found->second) {
        bool skip = false;
        for (const std::string& d : rule.exclude) skip = skip || HostIsOrUnder(host, d);
        if (!skip) rules->selectors.push_back(rule.selector);
      }
    }
    const size_t dot = host.find('.', at);
    if (dot == std::string::npos) break;
    at = dot + 1;
  }
  for (const std::string& selector : excepted) {
    if (selector[0] == '#') rules->exceptIds.push_back(selector.substr(1));
    else if (selector[0] == '.') rules->exceptClasses.push_back(selector.substr(1));
    rules->selectors.erase(std::remove(rules->selectors.begin(), rules->selectors.end(), selector), rules->selectors.end());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (cache_.size() > 512) cache_.clear();
  return cache_.emplace(host, std::move(rules)).first->second;
}

}  // namespace solar::net
