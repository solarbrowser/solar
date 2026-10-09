#pragma once

#include <array>
#include <optional>
#include <string>

#include "solar/css/Syntax.h"

// CSS Color (https://drafts.csswg.org/css-color/): the <color> type, its spellings and how a specified color is serialized.
namespace solar::css {

// The specified value of a <color>: keywords lowercased, hex colors, rgb(), hsl() and hwb() resolved to rgb()/rgba(),
// the other functions with their numbers in the form the standard gives them. Nothing if `value` is not a color.
std::optional<ComponentValue> NormalizeColor(const ComponentValue& value);

// The computed value of a color that is already a specified (normalized) value: legacy colors and keywords as rgb()/rgba(),
// currentcolor as `currentColor` (an rgb() text), system colors in the given scheme.
std::optional<ComponentValue> ComputeColor(const ComponentValue& specified, const std::string& currentColor, const std::string& scheme);

}  // namespace solar::css
