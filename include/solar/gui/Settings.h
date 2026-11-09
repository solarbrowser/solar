/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

// What the user has chosen for the browser, kept in a file of the user's (~/.config/solar/settings.conf, a line `key=value` each). The browser's process holds them; the settings page (a renderer of
// its own, solar://settings/) asks to change one and is told how they are. A value that is none of the ones a setting may have is not taken.
namespace solar::gui {

struct SearchEngine {
  const char* id;
  const char* name;
  const char* address;  // what the words typed are put after
};

class Settings {
 public:
  static Settings& Instance();

  // The value of a setting (its default where the user has not chosen).
  std::string Get(const std::string& key) const;
  // False where the key is none, or the value is not one the setting may have. The file is written.
  bool Set(const std::string& key, const std::string& value);
  // All of them for the settings page, as JSON: {"theme":"system", ..., "engines":[{"id","name"}]}.
  std::string Json() const;
  // Where the words typed in the address bar go for a search.
  std::string SearchAddress() const;
  // The address a window starts at when it is not given one: the page of a new tab, or the one the user chose.
  std::string StartAddress() const;
  // Whether the window has the system's bar (a setting, and SOLAR_SYSTEM_TITLE=1 which overrides it).
  bool SystemTitleBar() const;

  static const std::vector<SearchEngine>& Engines();
  // Where the file is (SOLAR_SETTINGS_FILE for tests).
  static std::string Path();

 private:
  Settings();
  void Save() const;

  mutable std::mutex mutex_;
  std::map<std::string, std::string> values_;
};

}  // namespace solar::gui
