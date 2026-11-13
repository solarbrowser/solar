/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace solar::dom {
struct Document;
struct Element;
}

// What the content blocker has an element hidden by (the cosmetic rules of the filter lists): for a document of a host, the ids and classes that hide an element wherever it is (the sets are the
// lists' and are shared by the documents; what the host has made an exception of is in `except...`), and the selectors that are for the host. The style of an element has display: none
// !important where one of them is for it.
namespace solar::css {

struct HideRules {
  const std::unordered_set<std::string>* ids = nullptr;
  const std::unordered_set<std::string>* classes = nullptr;
  std::vector<std::string> exceptIds, exceptClasses;
  std::vector<std::string> selectors;  // each a selector (not a list)
};

// What the page's document is to have hidden (none: nothing is). Set once by the renderer, with SetContentHiding to turn it on and off.
using HideProvider = const HideRules* (*)(dom::Document* document);
void SetHideProvider(HideProvider provider);
void SetContentHiding(bool on);

struct MatchContext;
// Whether the blocker has the element hidden (its style then has display: none !important).
bool HiddenByBlocker(dom::Element* element, const MatchContext& context);

}  // namespace solar::css
