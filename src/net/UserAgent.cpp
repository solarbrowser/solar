#include "solar/net/UserAgent.h"

namespace solar::net {

namespace {

// How each platform names the processors it runs on, which is why they differ below.
#if defined(__x86_64__) || defined(_M_X64)
#define SOLAR_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SOLAR_ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
#define SOLAR_ARCH_ARM32 1
#elif defined(__i386__) || defined(_M_IX86)
#define SOLAR_ARCH_X86 1
#endif

const char* PlatformToken() {
#if defined(_WIN32)
#if defined(SOLAR_ARCH_X64)
  return "Windows NT 10.0; Win64; x64";
#elif defined(SOLAR_ARCH_ARM64)
  return "Windows NT 10.0; Win64; ARM64";
#else
  return "Windows NT 10.0";
#endif
#elif defined(__APPLE__)
#if defined(SOLAR_ARCH_ARM64)
  return "Macintosh; macOS arm64";
#else
  return "Macintosh; macOS x86_64";
#endif
#else
#if defined(SOLAR_ARCH_X64)
  return "X11; Linux x86_64";
#elif defined(SOLAR_ARCH_ARM64)
  return "X11; Linux aarch64";
#elif defined(SOLAR_ARCH_ARM32)
  return "X11; Linux armv7l";
#elif defined(SOLAR_ARCH_X86)
  return "X11; Linux i686";
#else
  return "X11; Linux";
#endif
#endif
}

}  // namespace

std::string DefaultUserAgent() {
  return std::string("Solar/Developer (") + PlatformToken() + "; rv:development) Solar/Development Quanta/1.0";
}

}  // namespace solar::net
