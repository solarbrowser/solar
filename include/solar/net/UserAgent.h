#pragma once

#include <string>

namespace solar::net {

// The User-Agent a client sends when it has not been given one. It says what Solar is and what it
// runs on, and nothing else:
// The part in parentheses is the system the build was made for: the operating system and, where it
// is not the usual one, the processor, so an ARM build says so.
std::string DefaultUserAgent();

}  // namespace solar::net
