#include "solar/net/UserAgent.h"

namespace solar::net {

namespace {

// The Chrome version claimed. Sites pick what to serve by it, so it should not fall far behind
// the real one; it wants raising now and then.
constexpr int kClaimedChromeMajor = 145;

// Chrome freezes this part of its User-Agent, so these are the same whatever the real system is.
const char* PlatformToken() {
#if defined(_WIN32)
  return "Windows NT 10.0; Win64; x64";
#elif defined(__APPLE__)
  return "Macintosh; Intel Mac OS X 10_15_7";
#else
  return "X11; Linux x86_64";
#endif
}

}  // namespace

std::string DefaultUserAgent() {
  return std::string("Mozilla/5.0 (") + PlatformToken() + ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" +
         std::to_string(kClaimedChromeMajor) + ".0.0.0 Safari/537.36 Solar/Developer";
}

}  // namespace solar::net
