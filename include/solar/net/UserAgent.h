#pragma once

#include <string>

namespace solar::net {

// The User-Agent a client sends when it has not been given one. It claims to be Chrome, because
// sites decide what to serve by that token, and says what it really is at the end:
//   Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0
//   Safari/537.36 Solar/Developer
std::string DefaultUserAgent();

}  // namespace solar::net
