#pragma once

#include <optional>
#include <string_view>

#include "solar/url/Url.h"

namespace solar::url {

// The WHATWG URL Standard's URL parser. Input that is not valid UTF-8 is
// decoded with replacement characters first, as the standard expects of callers.
std::optional<Url> Parse(std::string_view input, const Url* base = nullptr);

}  // namespace solar::url
