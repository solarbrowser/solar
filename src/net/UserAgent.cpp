/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/UserAgent.h"

#include <cstdlib>
#include <string>

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
  // (As every browser says it on a Mac, whatever the processor and the version: it is frozen.)
  return "Macintosh; Intel Mac OS X 10_15_7";
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

// The major version of Chrome that the page is told Solar is like. Sites that look at the User-Agent (Google's sign-in script answers 403 to one it does not know) look for the engine's name
// (AppleWebKit and Chrome, or Gecko and Firefox) and for a version that is recent; the third part of a version is no longer sent by Chrome, so it is not here.
constexpr int kChromeMajor = 142;

std::string DefaultUserAgent() {
  // SOLAR_USER_AGENT: another, for trying what a site does with it.
  if (const char* given = std::getenv("SOLAR_USER_AGENT")) {
    if (*given) return given;
  }
  // The way Chrome says it, and what Solar is after it as the browsers that are made of Chrome's engine do it (Edge has Edg/, Opera OPR/).
  return std::string("Mozilla/5.0 (") + PlatformToken() + ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + std::to_string(kChromeMajor) + ".0.0.0 Safari/537.36 Solar/1.0";
}

std::string NavigatorPlatform() {
#if defined(_WIN32)
  return "Win32";
#elif defined(__APPLE__)
  return "MacIntel";
#elif defined(SOLAR_ARCH_X64)
  return "Linux x86_64";
#elif defined(SOLAR_ARCH_ARM64)
  return "Linux aarch64";
#else
  return "Linux";
#endif
}

}  // namespace solar::net
