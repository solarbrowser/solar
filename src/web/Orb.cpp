/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web/Orb.h"

#include <cctype>
#include <string>

#include "solar/web/MimeType.h"

namespace solar::web {

namespace {

bool EndsWith(const std::string& text, const char* suffix) {
  const std::string tail(suffix);
  return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

bool IsJavaScriptEssence(const std::string& essence) {
  for (const char* name : {"text/javascript", "application/javascript", "application/x-javascript", "application/ecmascript", "text/ecmascript", "text/jscript", "text/livescript",
                           "text/x-javascript", "text/x-ecmascript", "application/x-ecmascript", "text/javascript1.0", "text/javascript1.1", "text/javascript1.2",
                           "text/javascript1.3", "text/javascript1.4", "text/javascript1.5", "text/jscript", "text/livescript"}) {
    if (essence == name) return true;
  }
  return false;
}

// A JSON text, whole (RFC 8259), by recursive descent.
class Json {
 public:
  explicit Json(std::string_view text) : text_(text) {}
  bool Valid() {
    Space();
    if (!Value(0)) return false;
    Space();
    return position_ == text_.size();
  }

 private:
  void Space() {
    while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' || text_[position_] == '\r')) ++position_;
  }
  bool Literal(const char* word) {
    const std::string_view w(word);
    if (text_.substr(position_, w.size()) != w) return false;
    position_ += w.size();
    return true;
  }
  bool String() {
    if (position_ >= text_.size() || text_[position_] != '"') return false;
    ++position_;
    while (position_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[position_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c == '\\') {
        if (position_ >= text_.size()) return false;
        const char e = text_[position_++];
        if (e == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (position_ >= text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[position_++]))) return false;
          }
        } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) {
          return false;
        }
      }
    }
    return false;
  }
  bool Number() {
    const size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return false;
    if (text_[position_] == '0') ++position_;
    else if (text_[position_] >= '1' && text_[position_] <= '9') { while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_; }
    else return false;
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      const size_t digits = position_;
      while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
      if (position_ == digits) return false;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      const size_t digits = position_;
      while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
      if (position_ == digits) return false;
    }
    return position_ > start;
  }
  bool Value(int depth) {
    if (depth > 512 || position_ >= text_.size()) return false;
    switch (text_[position_]) {
      case '{': {
        ++position_;
        Space();
        if (position_ < text_.size() && text_[position_] == '}') { ++position_; return true; }
        while (true) {
          Space();
          if (!String()) return false;
          Space();
          if (position_ >= text_.size() || text_[position_++] != ':') return false;
          Space();
          if (!Value(depth + 1)) return false;
          Space();
          if (position_ >= text_.size()) return false;
          if (text_[position_] == ',') { ++position_; continue; }
          if (text_[position_] == '}') { ++position_; return true; }
          return false;
        }
      }
      case '[': {
        ++position_;
        Space();
        if (position_ < text_.size() && text_[position_] == ']') { ++position_; return true; }
        while (true) {
          Space();
          if (!Value(depth + 1)) return false;
          Space();
          if (position_ >= text_.size()) return false;
          if (text_[position_] == ',') { ++position_; continue; }
          if (text_[position_] == ']') { ++position_; return true; }
          return false;
        }
      }
      case '"': return String();
      case 't': return Literal("true");
      case 'f': return Literal("false");
      case 'n': return Literal("null");
      default: return Number();
    }
  }

  std::string_view text_;
  size_t position_ = 0;
};

}  // namespace

OrbVerdict OrbCheckHead(int status, std::string_view contentType, bool nosniff, std::string_view contentRange) {
  // A part of a resource that does not start at its start is not one a page may have of another site.
  if (status == 206 && !contentRange.empty()) {
    size_t i = contentRange.find("bytes");
    if (i != std::string_view::npos) {
      i += 5;
      while (i < contentRange.size() && (contentRange[i] == ' ' || contentRange[i] == '=')) ++i;
      if (i < contentRange.size() && contentRange[i] != '0') return OrbVerdict::Block;
    }
  }
  const auto mime = contentType.empty() ? std::nullopt : ParseMimeType(contentType);
  if (!mime) return nosniff ? OrbVerdict::Block : OrbVerdict::NeedBody;
  const std::string essence = mime->type + "/" + mime->subtype;
  if (mime->type == "image" || mime->type == "audio" || mime->type == "video" || essence == "text/css" || IsJavaScriptEssence(essence)) return OrbVerdict::Allow;
  const bool sniffed = essence == "text/html" || essence == "text/xml" || essence == "application/xml" || essence == "application/json" || essence == "text/json" ||
                       EndsWith(mime->subtype, "+json") || EndsWith(mime->subtype, "+xml");
  if (sniffed && !nosniff && status != 206) return OrbVerdict::NeedBody;
  return OrbVerdict::Block;
}

bool OrbBodyIsBlocked(std::string_view body) {
  if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
  size_t start = 0;
  while (start < body.size() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\n' || body[start] == '\r' || body[start] == '\f')) ++start;
  const std::string_view text = body.substr(start);
  for (const char* breaker : {")]}'", "{}&&", "for(;;);"}) {
    if (text.starts_with(breaker)) return true;
  }
  std::string head(text.substr(0, 16));
  for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (head.starts_with("<?xml")) return true;
  if (head.starts_with("<!--")) return true;
  for (const char* tag : {"<!doctype html", "<html", "<head", "<script", "<iframe", "<h1", "<div", "<font", "<table", "<a", "<style", "<title", "<b", "<body", "<br", "<p"}) {
    const std::string t(tag);
    if (head.starts_with(t) && head.size() > t.size() && (head[t.size()] == ' ' || head[t.size()] == '>')) return true;
  }
  return Json(body).Valid();
}

}  // namespace solar::web
