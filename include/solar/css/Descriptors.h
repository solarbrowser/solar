#pragma once

#include <optional>
#include <string>

#include "solar/css/Syntax.h"

// The descriptors of the at-rules that have them (@font-face, @font-palette-values): their names, and what values they take.
namespace solar::css {

enum class DescriptorSet { FontFace, FontPaletteValues, CounterStyle };

// The name a descriptor goes by in its rule (font-width is font-stretch there), lowercased; nothing if the rule has no such descriptor.
std::optional<std::string> DescriptorName(DescriptorSet set, const std::string& name);
// Whether any at-rule has a descriptor of this name (what a style declaration has accessors for).
bool IsAnyDescriptorName(const std::string& name);
// The value of the descriptor in its canonical text, nothing when it is not a valid one.
std::optional<std::string> DescriptorValue(DescriptorSet set, const std::string& name, const ComponentValues& value);

}  // namespace solar::css
