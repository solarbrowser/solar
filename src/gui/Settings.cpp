/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "solar/gui/Chrome.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::gui {

namespace {

const std::map<std::string, std::string>& Defaults() {
  static const std::map<std::string, std::string> defaults = {
      {"theme", "system"},      // system, light, dark
      {"search", "duckduckgo"},  // the id of a search engine
      {"start", ""},             // an address; empty: the page of a new tab
      {"tabs", "top"},           // top: the tabs along the top of the window; side: in a column at its side; sidebar: the column has the buttons and the address as well
      {"downloads", ""},         // the folder files are kept in; empty: the system's folder of downloads
      {"downloadAsk", "off"},    // on: each file is asked where it is to be kept
      {"blockAds", "on"},        // on: the requests of advertisements and trackers (the lists of the content blocker) are refused
      {"titlebar", "app"},       // app: the browser draws the buttons of the window; system: the system's bar
  };
  return defaults;
}

std::string JsonText(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

}  // namespace

const std::vector<SearchEngine>& Settings::Engines() {
  static const std::vector<SearchEngine> engines = {
      {"duckduckgo", "DuckDuckGo", "https://html.duckduckgo.com/html/?q="},
      {"google", "Google", "https://www.google.com/search?q="},
      {"bing", "Bing", "https://www.bing.com/search?q="},
      {"brave", "Brave Search", "https://search.brave.com/search?q="},
      {"ecosia", "Ecosia", "https://www.ecosia.org/search?q="},
  };
  return engines;
}

std::string Settings::Path() {
  if (const char* forced = std::getenv("SOLAR_SETTINGS_FILE")) return forced;
#if defined(_WIN32)
  // %LOCALAPPDATA%\\solar (where the other files of the browser are).
  if (const char* local = std::getenv("LOCALAPPDATA")) return std::string(local) + "\\solar\\settings.conf";
  return "settings.conf";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) return std::string(home) + "/Library/Application Support/solar/settings.conf";
  return "settings.conf";
#else
  if (const char* config = std::getenv("XDG_CONFIG_HOME")) return std::string(config) + "/solar/settings.conf";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.config/solar/settings.conf";
#endif
}

Settings& Settings::Instance() {
  static Settings settings;
  return settings;
}

Settings::Settings() : values_(Defaults()) {
  std::ifstream file(Path());
  std::string line;
  while (std::getline(file, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
    // (A file that was edited by hand may hold what a setting does not take: the default stays.)
    if (Defaults().count(key)) {
      const std::string kept = values_[key];
      values_[key] = value;
      if (!Set(key, value)) values_[key] = kept;
    }
  }
}

std::string Settings::Get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = values_.find(key);
  return found == values_.end() ? "" : found->second;
}

bool Settings::Set(const std::string& key, const std::string& rawValue) {
  if (!Defaults().count(key) || rawValue.size() > 2048 || rawValue.find_first_of("\n\r") != std::string::npos) return false;
  std::string value = rawValue;
  if (key == "theme") {
    if (value != "system" && value != "light" && value != "dark") return false;
  } else if (key == "search") {
    bool known = false;
    for (const SearchEngine& engine : Engines()) known = known || value == engine.id;
    if (!known) return false;
  } else if (key == "downloadAsk" || key == "blockAds") {
    if (value != "on" && value != "off") return false;
  } else if (key == "downloads") {
    // (A folder as a full path, or from the user's own: ~/Downloads/Solar. Empty is the system's.)
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (value.rfind("~/", 0) == 0) {
      const char* home = std::getenv("HOME");
      if (!home) return false;
      value = std::string(home) + value.substr(1);
    }
    const bool absolute = value.empty() || value[0] == '/' || (value.size() > 2 && value[1] == ':' && (value[2] == '\\' || value[2] == '/'));
    if (!absolute || value.find("..") != std::string::npos) return false;
  } else if (key == "tabs") {
    if (value != "top" && value != "side" && value != "sidebar") return false;
  } else if (key == "titlebar") {
    if (value != "app" && value != "system") return false;
  } else if (key == "start") {
    // (What was typed is made an address as the address bar does; empty is the page of a new tab.)
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (!value.empty()) {
      if (value.rfind("solar://", 0) != 0) value = AddressFromInput(value, "https://html.duckduckgo.com/html/?q=");
      const std::optional<url::Url> parsed = url::Parse(value);
      if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https" && parsed->scheme != "file" && parsed->scheme != "solar")) return false;
      value = url::Serialize(*parsed);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
  }
  Save();
  return true;
}

void Settings::Save() const {
  std::map<std::string, std::string> copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    copy = values_;
  }
  const std::filesystem::path path = Path();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;
    for (const auto& [key, value] : copy) out << key << '=' << value << '\n';
  }
  std::filesystem::rename(temporary, path, error);
}

std::string Settings::Json() const {
  std::map<std::string, std::string> copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    copy = values_;
  }
  std::string json = "{\"settings\":{";
  bool first = true;
  for (const auto& [key, value] : copy) {
    if (!first) json += ",";
    first = false;
    json += JsonText(key) + ":" + JsonText(value);
  }
  json += "},\"engines\":[";
  first = true;
  for (const SearchEngine& engine : Engines()) {
    if (!first) json += ",";
    first = false;
    json += std::string("{\"id\":") + JsonText(engine.id) + ",\"name\":" + JsonText(engine.name) + "}";
  }
  return json + "]}";
}

std::string Settings::SearchAddress() const {
  const std::string chosen = Get("search");
  for (const SearchEngine& engine : Engines()) {
    if (chosen == engine.id) return engine.address;
  }
  return Engines().front().address;
}

std::string Settings::StartAddress() const {
  const std::string start = Get("start");
  return start.empty() ? kNewTabAddress : start;
}

std::string Settings::DownloadDirectory() const { return Get("downloads"); }

bool Settings::SystemTitleBar() const {
  const char* forced = std::getenv("SOLAR_SYSTEM_TITLE");
  if (forced && *forced) return std::strcmp(forced, "0") != 0;
  return Get("titlebar") == "system";
}

}  // namespace solar::gui
