#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace solar::html {

// A named character reference of the HTML Standard: what is written after the "&" (with the ";" if it
// has one: the legacy ones are in the table both ways), and the code points it stands for.
struct NamedEntity {
  const char* name;
  uint32_t first;
  uint32_t second;  // 0: just the one
};

// Sorted by name. Generated: see tools/GenEntities.cpp.
extern const NamedEntity kNamedEntities[];
extern const size_t kNamedEntityCount;

// The entities whose names begin with `prefix`, as the half-open range of the table that holds them.
// The tokenizer asks it after each character to see whether a reference can still be matched.
std::pair<size_t, size_t> EntitiesWithPrefix(std::string_view prefix);
// The entity of exactly this name, or null.
const NamedEntity* FindEntity(std::string_view name);

}  // namespace solar::html
