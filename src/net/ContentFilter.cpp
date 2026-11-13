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
