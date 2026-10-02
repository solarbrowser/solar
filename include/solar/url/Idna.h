#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace solar::url {

// The URL Standard's "domain parser" with beStrict false, the only mode the host parser uses.
std::optional<std::string> DomainToAscii(std::u32string_view domain);

}  // namespace solar::url
