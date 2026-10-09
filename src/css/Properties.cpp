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

}  // namespace

const PropertyDefinition* FindProperty(std::string_view name) {
  const auto found = PropertyIndex().find(name);
  return found == PropertyIndex().end() ? nullptr : found->second;
}

const char* FindTypeSyntax(std::string_view name) {
  const auto found = TypeIndex().find(name);
  return found == TypeIndex().end() ? nullptr : found->second;
}

}  // namespace solar::css
