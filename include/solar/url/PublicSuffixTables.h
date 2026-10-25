/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>

// Declarations for the table in PublicSuffixTables.cpp, which tools/GenPublicSuffix.cpp generates
// from the Public Suffix List. The rules are sorted by key so lookups can binary search.
namespace solar::url::psl {

// What the list has for one key, a domain without the "*." or "!" its rule may have started with.
enum RuleFlag : uint8_t {
  kRule = 1,       // "key" is a public suffix
  kWildcard = 2,   // "*.key": every one-label extension of key is a public suffix
  kException = 4,  // "!key": key is not a public suffix, although a wildcard says it is
};

struct Rule {
  const char* key;
  uint8_t flags;
};

extern const Rule kRules[];
extern const size_t kRuleCount;

}  // namespace solar::url::psl
