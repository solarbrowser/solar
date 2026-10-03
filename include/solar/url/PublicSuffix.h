#pragma once

#include <string_view>

namespace solar::url {

// The public suffix of `host`, the part of a name under which anyone can register their own: "com",
// "co.uk", "github.io". `host` is a host as a URL serializes it, in lower case with any non-ASCII
// label already in punycode. Empty for an IP address, an empty host and a name with an empty label. The view is into `host`.
std::string_view PublicSuffix(std::string_view host);

// The public suffix and the one label before it, "example.co.uk": the part of a name that one
// registrant owns. Empty if `host` is itself a public suffix, and for an IP address.
std::string_view RegistrableDomain(std::string_view host);

}  // namespace solar::url
