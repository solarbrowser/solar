#pragma once

#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "solar/url/Utf8.h"

namespace solar::test {

struct Json {
  enum class Type { Null, Bool, String, Array, Object } type = Type::Null;
  bool boolean = false;
  std::string string;
  std::vector<Json> array;
  std::map<std::string, Json> object;

  const Json* Find(const std::string& key) const {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  Json Read() {
    SkipSpace();
    char c = text_.at(pos_);
    Json value;
    if (c == '{') {
      value.type = Json::Type::Object;
      ++pos_;
      SkipSpace();
      while (text_[pos_] != '}') {
        std::string key = ReadString();
        SkipSpace();
        ++pos_;  // ':'
        value.object[key] = Read();
        SkipSpace();
        if (text_[pos_] == ',') ++pos_;
        SkipSpace();
      }
      ++pos_;
    } else if (c == '[') {
      value.type = Json::Type::Array;
      ++pos_;
      SkipSpace();
      while (text_[pos_] != ']') {
        value.array.push_back(Read());
        SkipSpace();
        if (text_[pos_] == ',') ++pos_;
        SkipSpace();
      }
      ++pos_;
    } else if (c == '"') {
      value.type = Json::Type::String;
      value.string = ReadString();
    } else if (text_.compare(pos_, 4, "true") == 0) {
      value.type = Json::Type::Bool;
      value.boolean = true;
      pos_ += 4;
    } else if (text_.compare(pos_, 5, "false") == 0) {
      value.type = Json::Type::Bool;
      pos_ += 5;
    } else {
      pos_ += 4;  // null
    }
    return value;
  }

 private:
  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                   text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  unsigned ReadHex4() {
    unsigned value = std::stoul(text_.substr(pos_, 4), nullptr, 16);
    pos_ += 4;
    return value;
  }

  // Test inputs hold lone surrogates, which the standard's callers turn into U+FFFD.
  std::string ReadString() {
    std::string out;
    ++pos_;  // opening quote
    while (text_[pos_] != '"') {
      char c = text_[pos_++];
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      char escape = text_[pos_++];
      switch (escape) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          unsigned unit = ReadHex4();
          char32_t codePoint = unit;
          if (unit >= 0xD800 && unit <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
            size_t saved = pos_;
            pos_ += 2;
            unsigned low = ReadHex4();
            if (low >= 0xDC00 && low <= 0xDFFF) {
              codePoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = saved;
              codePoint = 0xFFFD;
            }
          } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            codePoint = 0xFFFD;
          }
          solar::url::AppendUtf8(out, codePoint);
          break;
        }
        default: out.push_back(escape);
      }
    }
    ++pos_;
    return out;
  }

  const std::string& text_;
  size_t pos_ = 0;
};


inline std::optional<Json> Load(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string text = buffer.str();
  return JsonReader(text).Read();
}

}  // namespace solar::test
