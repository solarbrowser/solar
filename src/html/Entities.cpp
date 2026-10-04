#include "solar/html/Entities.h"

#include <algorithm>
#include <string_view>

namespace solar::html {

std::pair<size_t, size_t> EntitiesWithPrefix(std::string_view prefix) {
  const NamedEntity* begin = kNamedEntities;
  const NamedEntity* end = kNamedEntities + kNamedEntityCount;
  const NamedEntity* first = std::lower_bound(begin, end, prefix, [](const NamedEntity& entity, std::string_view value) { return std::string_view(entity.name) < value; });
  const NamedEntity* last = first;
  while (last != end && std::string_view(last->name).starts_with(prefix)) ++last;
  return {static_cast<size_t>(first - begin), static_cast<size_t>(last - begin)};
}

const NamedEntity* FindEntity(std::string_view name) {
  const auto [first, last] = EntitiesWithPrefix(name);
  for (size_t i = first; i < last; ++i) {
    if (std::string_view(kNamedEntities[i].name) == name) return &kNamedEntities[i];
  }
  return nullptr;
}

}  // namespace solar::html
