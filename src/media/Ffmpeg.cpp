/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "FfmpegApi.h"

#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace solar::media {

FfmpegApi g_api;

namespace {

#define SOLAR_STR2(x) #x
#define SOLAR_STR(x) SOLAR_STR2(x)

// The name a platform gives a shared library of one major version.
std::string LibraryName(const char* base, int major) {
#if defined(_WIN32)
  return std::string(base) + "-" + std::to_string(major) + ".dll";
#elif defined(__APPLE__)
  return std::string("lib") + base + "." + std::to_string(major) + ".dylib";
#else
  return std::string("lib") + base + ".so." + std::to_string(major);
#endif
}

void* Open(const std::string& name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryA(name.c_str()));
#else
  return dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Symbol(void* library, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}

bool Load() {
  struct Library {
    const char* base;
    int major;
    void* handle;
  } libraries[] = {
      {"avutil", LIBAVUTIL_VERSION_MAJOR, nullptr},     {"swresample", LIBSWRESAMPLE_VERSION_MAJOR, nullptr}, {"swscale", LIBSWSCALE_VERSION_MAJOR, nullptr},
      {"avcodec", LIBAVCODEC_VERSION_MAJOR, nullptr},   {"avformat", LIBAVFORMAT_VERSION_MAJOR, nullptr},
  };
  for (Library& library : libraries) {
    library.handle = Open(LibraryName(library.base, library.major));
    if (!library.handle) return false;
  }
  auto find = [&](const char* base) -> void* {
    for (const Library& library : libraries) {
      if (std::string(library.base) == base) return library.handle;
    }
    return nullptr;
  };
  bool good = true;
#define X(lib, name)                                                          \
  g_api.name = reinterpret_cast<decltype(g_api.name)>(Symbol(find(#lib), #name)); \
  if (!g_api.name) good = false;
  SOLAR_FFMPEG_FUNCTIONS(X)
#undef X
  return good;
}

}  // namespace

bool LoadFfmpeg() {
  static std::once_flag once;
  static bool loaded = false;
  std::call_once(once, [] { loaded = Load(); });
  return loaded;
}

}  // namespace solar::media
