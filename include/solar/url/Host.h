#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "solar/url/ValidationError.h"

namespace solar::url {

// Returns the serialized host. `input` must be valid UTF-8.
std::optional<std::string> ParseHost(std::string_view input, bool isOpaque, ValidationErrors* errors = nullptr);

}  // namespace solar::url
