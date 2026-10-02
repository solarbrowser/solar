#pragma once

#include <optional>
#include <string_view>

#include "solar/url/Url.h"

namespace solar::url {

// The WHATWG URL Standard's URL parser. Input that is not valid UTF-8 is
// decoded with replacement characters first, as the standard expects of callers.
std::optional<Url> Parse(std::string_view input, const Url* base = nullptr);

// The states the URL setters start the parser in ("state override").
enum class StateOverride { SchemeStart, Host, Hostname, Port, PathStart, Query, Fragment };

// Parses `input` into `url` in place. Returns false where the standard says "return
// failure"; the setters ignore that, and `url` is untouched by a failed host or port.
bool ParseInto(Url& url, std::string_view input, StateOverride state);

}  // namespace solar::url
