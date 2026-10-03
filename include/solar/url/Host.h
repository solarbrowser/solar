#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "solar/url/ValidationError.h"

namespace solar::url {

// Returns the serialized host. `input` must be valid UTF-8.
std::optional<std::string> ParseHost(std::string_view input, bool isOpaque, ValidationErrors* errors = nullptr);

// Whether a serialized host is an IP address: an IPv6 one is bracketed, and an IPv4 one is the only
// host made of digits and dots alone, since a domain whose last label is a number parses as IPv4.
bool IsIpAddressHost(std::string_view host);

}  // namespace solar::url
