/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

// What differs between the systems in how a program finds itself and starts another: the one place that says it.
namespace solar::gui {

// Where the program's file is (empty if it cannot be found).
inline std::string ExecutablePath() {
#if defined(_WIN32)
  char buffer[MAX_PATH * 4];
  const DWORD length = ::GetModuleFileNameA(nullptr, buffer, sizeof buffer);
  return length > 0 && length < sizeof buffer ? std::string(buffer, length) : std::string();
#elif defined(__APPLE__)
  char buffer[PATH_MAX * 2];
  uint32_t size = sizeof buffer;
  if (_NSGetExecutablePath(buffer, &size) != 0) return "";
  char real[PATH_MAX];
  return ::realpath(buffer, real) ? std::string(real) : std::string(buffer);
#else
  char buffer[4096];
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
  return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string();
#endif
}

// Sets a variable of the environment (what the program says to a library it opens).
inline void SetEnvironment(const char* name, const char* value, bool overwrite) {
#if defined(_WIN32)
  if (overwrite || !std::getenv(name)) _putenv_s(name, value);
#else
  ::setenv(name, value, overwrite ? 1 : 0);
#endif
}

#if !defined(_WIN32)
// Starts the program `path` with what `actions` give it (descriptors put at places by dup2), and with every other descriptor closed from `closeFrom` on: Linux has an action for that;
// macOS closes all that are not given a place (the three standard ones are kept).
inline int SpawnKeepingOnly(pid_t* child, const char* path, posix_spawn_file_actions_t* actions, int closeFrom, char* const arguments[]) {
#if defined(__APPLE__)
  (void)closeFrom;
  for (int standard = 0; standard < 3; ++standard) posix_spawn_file_actions_addinherit_np(actions, standard);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
  const int status = ::posix_spawn(child, path, actions, &attributes, arguments, *_NSGetEnviron());
  posix_spawnattr_destroy(&attributes);
  return status;
#else
  posix_spawn_file_actions_addclosefrom_np(actions, closeFrom);
  return ::posix_spawn(child, path, actions, nullptr, arguments, environ);
#endif
}
#endif

}  // namespace solar::gui
