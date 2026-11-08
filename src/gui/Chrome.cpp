/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Chrome.h"

#include <algorithm>
#include <cctype>

#include "solar/web/Scripts.h"

namespace solar::gui {

std::string ChromeDocument() {
  std::string text;
  for (const char* const* piece = solar::web::kScriptChromeUi; *piece; ++piece) text += *piece;
  return text;
}

std::string NewTabDocument() {
  std::string text;
  for (const char* const* piece = solar::web::kScriptNewTab; *piece; ++piece) text += *piece;
  return text;
}

std::string AddressFromInput(const std::string& input) {
  size_t first = 0, last = input.size();
  while (first < last && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  const std::string text = input.substr(first, last - first);
  if (text.empty()) return "";
  const auto startsWith = [&](const char* prefix) {
    const std::string p = prefix;
    return text.size() >= p.size() && std::equal(p.begin(), p.end(), text.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
  };
  if (startsWith("http://") || startsWith("https://") || startsWith("file://") || startsWith("about:") || startsWith("data:")) return text;
  const bool spaces = text.find(' ') != std::string::npos;
  if (!spaces) {
    const std::string host = text.substr(0, text.find_first_of("/?#"));
    const bool local = host == "localhost" || host.rfind("localhost:", 0) == 0;
    const bool address = host.find('.') != std::string::npos || host.find(':') != std::string::npos;
    if (local) return "http://" + text;
    if (address) {
      // (A number with dots, or a name with a dot in it: an address; a plain word is a search.)
      const bool digits = std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ':'; });
      return (digits ? "http://" : "https://") + text;
    }
  }
  std::string query;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') query += static_cast<char>(c);
    else if (c == ' ') query += '+';
    else {
      static const char* hex = "0123456789ABCDEF";
      query += '%';
      query += hex[c >> 4];
      query += hex[c & 15];
    }
  }
  return "https://html.duckduckgo.com/html/?q=" + query;
}

}  // namespace solar::gui
