/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web/MimeType.h"

#include <algorithm>

namespace solar::web {

namespace {

bool IsHttpWhitespace(char c) { return c == '\t' || c == '\n' || c == '\r' || c == ' '; }

bool IsTokenChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) return true;
  return u != 0 && std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool IsToken(std::string_view text) { return !text.empty() && std::all_of(text.begin(), text.end(), IsTokenChar); }

// The code points of a quoted string or an unquoted value: tab, the printable ASCII, and the bytes from 0x80.
bool IsQuotedStringTokenChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == '\t' || (u >= 0x20 && u <= 0x7E) || u >= 0x80;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

std::optional<MimeType> ParseMimeType(std::string_view input) {
  size_t first = 0, last = input.size();
  while (first < last && IsHttpWhitespace(input[first])) ++first;
  while (last > first && IsHttpWhitespace(input[last - 1])) --last;
  const std::string_view text = input.substr(first, last - first);
  const size_t slash = text.find('/');
  if (slash == std::string_view::npos || slash == 0) return std::nullopt;
  const std::string_view type = text.substr(0, slash);
  if (!IsToken(type)) return std::nullopt;
  size_t position = slash + 1;
  size_t subtypeEnd = text.find(';', position);
  std::string_view subtype = text.substr(position, subtypeEnd == std::string_view::npos ? std::string_view::npos : subtypeEnd - position);
  while (!subtype.empty() && IsHttpWhitespace(subtype.back())) subtype.remove_suffix(1);
  if (!IsToken(subtype)) return std::nullopt;
  MimeType mime{Lower(type), Lower(subtype), {}};
  if (subtypeEnd == std::string_view::npos) return mime;
  position = subtypeEnd + 1;
  while (position < text.size()) {
    while (position < text.size() && IsHttpWhitespace(text[position])) ++position;
    std::string name;
    while (position < text.size() && text[position] != ';' && text[position] != '=') name.push_back(text[position++]);
    name = Lower(name);
    if (position < text.size()) {
      if (text[position] == ';') { ++position; continue; }
      ++position;  // the =
    }
    if (position >= text.size()) break;
    std::string value;
    if (text[position] == '"') {
      ++position;
      while (position < text.size()) {
        if (text[position] == '\\') {
          ++position;
          if (position >= text.size()) { value.push_back('\\'); break; }
          value.push_back(text[position++]);
        } else if (text[position] == '"') {
          ++position;
          break;
        } else {
          value.push_back(text[position++]);
        }
      }
      while (position < text.size() && text[position] != ';') ++position;
    } else {
      while (position < text.size() && text[position] != ';') value.push_back(text[position++]);
      while (!value.empty() && IsHttpWhitespace(value.back())) value.pop_back();
      if (value.empty()) continue;
    }
    if (!name.empty() && IsToken(name) && std::all_of(value.begin(), value.end(), IsQuotedStringTokenChar) &&
        std::none_of(mime.parameters.begin(), mime.parameters.end(), [&](const auto& p) { return p.first == name; })) {
      mime.parameters.emplace_back(std::move(name), std::move(value));
    }
  }
  return mime;
}

std::string SerializeMimeType(const MimeType& mime) {
  std::string out = mime.type + "/" + mime.subtype;
  for (const auto& [name, value] : mime.parameters) {
    out += ";" + name + "=";
    if (!IsToken(value)) {
      out.push_back('"');
      for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
      }
      out.push_back('"');
    } else {
      out += value;
    }
  }
  return out;
}

}  // namespace solar::web
