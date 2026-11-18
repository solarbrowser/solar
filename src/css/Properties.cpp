/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/css/Properties.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace solar::css {

namespace {

const std::unordered_map<std::string_view, const PropertyDefinition*>& PropertyIndex() {
  static const auto index = [] {
    std::unordered_map<std::string_view, const PropertyDefinition*> map;
    for (size_t i = 0; i < kPropertyDefinitionCount; ++i) map.emplace(kPropertyDefinitions[i].name, &kPropertyDefinitions[i]);
    return map;
  }();
  return index;
}

const std::unordered_map<std::string_view, const char*>& TypeIndex() {
  static const auto index = [] {
    std::unordered_map<std::string_view, const char*> map;
    for (size_t i = 0; i < kTypeDefinitionCount; ++i) map.emplace(kTypeDefinitions[i].name, kTypeDefinitions[i].syntax);
    return map;
  }();
  return index;
}

// Legacy names for properties: the same property under another name.
const std::unordered_map<std::string_view, std::string_view>& Aliases() {
  static const std::unordered_map<std::string_view, std::string_view> aliases = {
      {"font-stretch", "font-width"},
      {"word-wrap", "overflow-wrap"},
      {"grid-gap", "gap"},
      {"grid-row-gap", "row-gap"},
      {"grid-column-gap", "column-gap"},
      {"-webkit-align-content", "align-content"},
      {"-webkit-align-items", "align-items"},
      {"-webkit-align-self", "align-self"},
      {"-webkit-appearance", "appearance"},
      {"-webkit-flex", "flex"},
      {"-webkit-flex-basis", "flex-basis"},
      {"-webkit-flex-direction", "flex-direction"},
      {"-webkit-flex-flow", "flex-flow"},
      {"-webkit-flex-grow", "flex-grow"},
      {"-webkit-flex-shrink", "flex-shrink"},
      {"-webkit-flex-wrap", "flex-wrap"},
      {"-webkit-justify-content", "justify-content"},
      {"-webkit-order", "order"},
  };
  return aliases;
}

}  // namespace

namespace {

// What a name is, in one probe of a flat table (every style asks for the properties by name, hundreds of times: two lookups in tables of nodes were most of what it cost). An alias answers
// for the property it stands for, as it did.
struct FlatIndex {
  struct Slot {
    std::string_view name;
    const PropertyDefinition* definition = nullptr;
  };
  static constexpr size_t kSize = 4096;  // (a power of two, with room: a probe ends at an empty slot soon)
  std::vector<Slot> slots = std::vector<Slot>(kSize);
  static uint64_t Hash(std::string_view text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
    return h;
  }
  void Put(std::string_view name, const PropertyDefinition* definition) {
    size_t at = Hash(name) & (kSize - 1);
    while (slots[at].definition && slots[at].name != name) at = (at + 1) & (kSize - 1);
    slots[at] = {name, definition};
  }
  const PropertyDefinition* Get(std::string_view name) const {
    size_t at = Hash(name) & (kSize - 1);
    while (slots[at].definition) {
      if (slots[at].name == name) return slots[at].definition;
      at = (at + 1) & (kSize - 1);
    }
    return nullptr;
  }
};

const FlatIndex& Flat() {
  static const FlatIndex index = [] {
    FlatIndex built;
    for (const auto& [name, definition] : PropertyIndex()) built.Put(name, definition);
    // (An alias is looked at before the property of that name, if there is one.)
    for (const auto& [name, target] : Aliases()) {
      const auto found = PropertyIndex().find(target);
      if (found != PropertyIndex().end()) built.Put(name, found->second);
    }
    return built;
  }();
  return index;
}

}  // namespace

const PropertyDefinition* FindProperty(std::string_view name) { return Flat().Get(name); }

const char* FindTypeSyntax(std::string_view name) {
  const auto found = TypeIndex().find(name);
  return found == TypeIndex().end() ? nullptr : found->second;
}

}  // namespace solar::css
