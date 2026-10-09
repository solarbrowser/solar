#include "solar/css/Properties.h"

#include <string>
#include <unordered_map>

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

const PropertyDefinition* FindProperty(std::string_view name) {
  if (const auto alias = Aliases().find(name); alias != Aliases().end()) name = alias->second;
  const auto found = PropertyIndex().find(name);
  return found == PropertyIndex().end() ? nullptr : found->second;
}

const char* FindTypeSyntax(std::string_view name) {
  const auto found = TypeIndex().find(name);
  return found == TypeIndex().end() ? nullptr : found->second;
}

}  // namespace solar::css
