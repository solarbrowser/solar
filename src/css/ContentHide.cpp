/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>

#include "solar/css/HideRules.h"
#include "solar/css/Selectors.h"
#include "solar/css/Style.h"
#include "solar/dom/Node.h"

namespace solar::css {

namespace {

HideProvider g_hideProvider = nullptr;
std::atomic<bool> g_hiding{false};

// The selectors of a HideRules, parsed once and found by the id, class or type of the end of each (so that an element is matched against the few that can be for it).
struct CompiledHides {
  std::vector<ComplexSelector> selectors;
  std::unordered_map<std::string, std::vector<uint32_t>> byId, byClass, byType;
  std::vector<uint32_t> always;
};

const CompiledHides& CompiledFor(const HideRules* rules) {
  static std::unordered_map<const HideRules*, std::unique_ptr<CompiledHides>> cache;
  auto found = cache.find(rules);
  if (found != cache.end()) return *found->second;
  if (cache.size() > 256) cache.clear();
  auto compiled = std::make_unique<CompiledHides>();
  for (const std::string& text : rules->selectors) {
    std::optional<SelectorList> list = ParseSelectorList(text);
    if (!list) continue;
    for (ComplexSelector& selector : *list) {
      if (selector.compounds.empty()) continue;
      const uint32_t at = static_cast<uint32_t>(compiled->selectors.size());
      const SimpleSelector* id = nullptr;
      const SimpleSelector* cls = nullptr;
      const SimpleSelector* type = nullptr;
      for (const SimpleSelector& simple : selector.compounds.back().simples) {
        if (simple.kind == SimpleSelector::Kind::Id && !id) id = &simple;
        else if (simple.kind == SimpleSelector::Kind::Class && !cls) cls = &simple;
        else if (simple.kind == SimpleSelector::Kind::Type && !type) type = &simple;
      }
      if (id) compiled->byId[id->name].push_back(at);
      else if (cls) compiled->byClass[cls->name].push_back(at);
      else if (type) compiled->byType[type->name].push_back(at);
      else compiled->always.push_back(at);
      compiled->selectors.push_back(std::move(selector));
    }
  }
  return *cache.emplace(rules, std::move(compiled)).first->second;
}

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> out;
  std::string word;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      if (!word.empty()) out.push_back(word);
      word.clear();
    } else {
      word += c;
    }
  }
  if (!word.empty()) out.push_back(word);
  return out;
}

}  // namespace

void SetHideProvider(HideProvider provider) { g_hideProvider = provider; }

void SetContentHiding(bool on) {
  if (g_hiding.exchange(on) != on) NoteStyleChange();
}

bool HiddenByBlocker(dom::Element* element, const MatchContext& context) {
  if (!g_hideProvider || !g_hiding.load(std::memory_order_relaxed) || !element->nodeDocument || !element->IsHtml()) return false;
  const HideRules* rules = g_hideProvider(element->nodeDocument);
  if (!rules) return false;
  const auto excluded = [](const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); };
  const dom::Attr* id = element->FindAttribute("", "id");
  if (id && rules->ids && rules->ids->count(id->Value()) && !excluded(rules->exceptIds, id->Value())) return true;
  const dom::Attr* classAttribute = element->FindAttribute("", "class");
  std::vector<std::string> classes;
  if (classAttribute) classes = Words(classAttribute->Value());
  if (rules->classes) {
    for (const std::string& name : classes) {
      if (rules->classes->count(name) && !excluded(rules->exceptClasses, name)) return true;
    }
  }
  if (rules->selectors.empty()) return false;
  const CompiledHides& compiled = CompiledFor(rules);
  const auto test = [&](const std::vector<uint32_t>& list) {
    for (uint32_t at : list) {
      if (MatchesComplex(compiled.selectors[at], element, context)) return true;
    }
    return false;
  };
  if (id) {
    const auto found = compiled.byId.find(id->Value());
    if (found != compiled.byId.end() && test(found->second)) return true;
  }
  for (const std::string& name : classes) {
    const auto found = compiled.byClass.find(name);
    if (found != compiled.byClass.end() && test(found->second)) return true;
  }
  const auto type = compiled.byType.find(element->localName);
  if (type != compiled.byType.end() && test(type->second)) return true;
  return test(compiled.always);
}

}  // namespace solar::css
