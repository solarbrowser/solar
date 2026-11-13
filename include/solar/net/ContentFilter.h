/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The network rules of the filter lists that block advertisements and trackers (Adblock Plus syntax: EasyList, EasyPrivacy, the cookie notice lists): `||host^`, `|start`, `end|`, `*`, `^`,
// `@@` exceptions, and the options third-party, the kinds of resource, and domain=. A rule with an option that is not understood (redirect, csp, removeparam, popup...) is left out, and
// so are the cosmetic rules (##) and the regular expressions: the filter blocks less than the list says, never more.
namespace solar::net {

enum FilterType : uint16_t {
  kFilterScript = 1,
  kFilterImage = 2,
  kFilterStyle = 4,
  kFilterXhr = 8,
  kFilterMedia = 16,
  kFilterFont = 32,
  kFilterSubdocument = 64,
  kFilterWebSocket = 128,
  kFilterPing = 256,
  kFilterOther = 512,
  kFilterAllTypes = 1023,
};

struct FilterQuery {
  std::string_view url;       // the whole address of the request
  std::string_view host;      // its host
  std::string_view pageHost;  // the host of the page that asks (empty: none)
  uint16_t type = kFilterOther;
  bool thirdParty = false;    // the request is for another site than the page's
};

class ContentFilter {
 public:
  // Reads the rules of a list (may be called for several lists). Finish() has to be called after the last, before Blocks.
  void AddList(std::string_view text);
  void Finish();

  // Whether a request is blocked: a rule matches it and no exception does. Safe to call from several threads once Finish has been called.
  bool Blocks(const FilterQuery& query) const;

  size_t RuleCount() const { return rules_.size(); }
  size_t UnindexedCount() const { return unindexed_.size(); }

 private:
  struct Rule {
    std::string pattern;  // lowercase, without the anchors; `*` and `^` are in it
    uint16_t types = kFilterAllTypes;
    uint8_t party = 0;    // 0: any, 1: third-party requests only, 2: first-party only
    bool anchorHost = false, anchorStart = false, anchorEnd = false, exception = false;
    uint32_t domains = 0;  // 1 + the index in domains_, 0: none
  };
  struct DomainSet {
    std::vector<std::string> include, exclude;
  };

  bool ParseLine(std::string_view line);
  bool Matches(const Rule& rule, const FilterQuery& query, const std::string& url) const;

  std::vector<Rule> rules_;
  std::vector<DomainSet> domains_;
  std::vector<std::pair<uint64_t, uint32_t>> index_;  // (hash of the key, rule), sorted by the hash
  std::vector<uint32_t> unindexed_;                   // rules with no key: tried for every request
};

}  // namespace solar::net
