// Form controls (https://html.spec.whatwg.org/multipage/rendering.html#form-controls): replaced elements, as large as the HTML standard
// and the engines that sized the tests make them by default.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Internal.h"
#include "solar/font/Face.h"

namespace solar::layout {

namespace {

std::string Attr(const dom::Element* element, const char* name) {
  const dom::Attr* a = element->FindAttribute(name);
  return a ? a->Value() : std::string();
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void CollectText(const dom::Node* node, std::string& out) {
  for (const dom::Node* child = dom::FirstChildOf(node); child; child = dom::NextSiblingOf(child)) {
    if (child->IsText()) out += static_cast<const dom::CharacterData*>(child)->Data();
    else if (child->IsElement()) CollectText(child, out);
  }
}

// The text with its white space collapsed, as the label of an option is.
std::string Collapsed(const std::string& text) {
  std::string out;
  bool space = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      space = !out.empty();
    } else {
      if (space) out += ' ';
      space = false;
      out += c;
    }
  }
  return out;
}

long PositiveInt(const std::string& s, long fallback) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  return end == s.c_str() || v <= 0 ? fallback : v;
}

}  // namespace

bool ControlSize(LayoutContext& lc, dom::Element* element, Box& box) {
  if (!element->IsHtml()) return false;
  const std::string& name = element->localName;
  if (name != "input" && name != "textarea" && name != "select" && name != "progress" && name != "meter") return false;
  const std::shared_ptr<font::Face> face = PrimaryFace(lc, box);
  const double size = box.style->fontSize;
  const auto widthOf = [&](const std::string& text) {
    return face ? face->Measure(text) * size : text.size() * size * 0.5;
  };
  // A line of the font: ascent and descent, rounded as the lines are.
  const double ascent = face ? std::round(face->metrics().ascent * size) : std::round(size * 0.8);
  const double descent = face ? std::round(face->metrics().descent * size) : std::round(size * 0.2);
  const double gap = face ? face->metrics().lineGap * size : 0;
  const double lineHeight = box.style->lineHeight.IsAuto() ? ascent + descent + gap : box.style->lineHeight.value;
  const double average = face ? std::max(widthOf("0"), 1.0) : size * 0.5;
  box.naturalRatio = 0;
  box.formControl = true;
  box.controlAscent = ascent + (lineHeight - ascent - descent) / 2;
  if (name == "progress") {
    box.naturalWidth = 160;
    box.naturalHeight = 16;
    box.controlAscent = -1;
    return true;
  }
  if (name == "meter") {
    box.naturalWidth = 80;
    box.naturalHeight = 16;
    box.controlAscent = -1;
    return true;
  }
  if (name == "textarea") {
    const long cols = PositiveInt(Attr(element, "cols"), 20), rows = PositiveInt(Attr(element, "rows"), 2);
    // (The width is that of a scrollbar more.)
    box.naturalWidth = std::round(cols * average) + 15;
    box.naturalHeight = rows * lineHeight;
    return true;
  }
  if (name == "select") {
    const bool multiple = element->FindAttribute("multiple") != nullptr;
    const long rows = PositiveInt(Attr(element, "size"), multiple ? 4 : 1);
    double widest = 0;
    const std::function<void(const dom::Node*)> walk = [&](const dom::Node* parent) {
      for (const dom::Node* child = dom::FirstChildOf(parent); child; child = dom::NextSiblingOf(child)) {
        if (!child->IsElement()) continue;
        const dom::Element* e = static_cast<const dom::Element*>(child);
        if (e->IsHtml("option")) {
          std::string label = Attr(e, "label");
          if (label.empty()) {
            CollectText(e, label);
            label = Collapsed(label);
          }
          widest = std::max(widest, widthOf(label));
        } else if (e->IsHtml("optgroup")) {
          walk(e);
        }
      }
    };
    walk(element);
    const double extra = rows > 1 || multiple ? 15 : 20;  // the scrollbar of a list, the arrow of a drop-down
    box.naturalWidth = std::round(widest) + extra;
    box.naturalHeight = rows * lineHeight;
    box.emptyWidth = extra;
    box.emptyHeight = rows * lineHeight;
    return true;
  }
  const std::string type = Lower(Attr(element, "type"));
  if (type == "hidden" || type == "image") {
    box.formControl = false;
    return false;
  }
  if (type == "checkbox" || type == "radio") {
    box.naturalWidth = box.naturalHeight = 13;
    box.controlAscent = -1;
    return true;
  }
  if (type == "range") {
    box.naturalWidth = 129;
    box.naturalHeight = 16;
    box.controlAscent = -1;
    return true;
  }
  if (type == "color") {
    box.naturalWidth = 44;
    box.naturalHeight = 23;
    box.controlAscent = -1;
    return true;
  }
  if (type == "button" || type == "submit" || type == "reset") {
    std::string label = Attr(element, "value");
    if (label.empty() && type != "button") label = type == "submit" ? "Submit" : "Reset";
    box.naturalWidth = std::round(widthOf(Collapsed(label)));
    box.naturalHeight = lineHeight;
    return true;
  }
  if (type == "file") {
    box.naturalWidth = std::round(widthOf("Choose File")) + std::round(widthOf("No file chosen")) + 8;
    box.naturalHeight = lineHeight;
    return true;
  }
  // Text and the kinds that look like it.
  const long chars = PositiveInt(Attr(element, "size"), 20);
  box.naturalWidth = std::round(chars * average);
  box.naturalHeight = lineHeight;
  return true;
}

size_t BytesBeforeUnit(const std::string& text, size_t units) {
  size_t at = 0, counted = 0;
  while (at < text.size() && counted < units) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    counted += length == 4 ? 2 : 1;
    at += length;
  }
  return std::min(at, text.size());
}

size_t UnitsBeforeByte(const std::string& text, size_t bytes) {
  size_t at = 0, counted = 0;
  bytes = std::min(bytes, text.size());
  while (at < bytes) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    counted += length == 4 ? 2 : 1;
    at += length;
  }
  return counted;
}

std::vector<TextLine> WrapText(const font::Face& face, double size, const std::string& text, double width) {
  std::vector<TextLine> lines;
  const auto measure = [&](size_t from, size_t to) { return face.Measure(text.substr(from, to - from)) * size; };
  size_t lineStart = 0;
  for (;;) {
    size_t newline = text.find('\n', lineStart);
    const size_t hardEnd = newline == std::string::npos ? text.size() : newline;
    size_t begin = lineStart;
    // The paragraph between two newlines, broken into as many lines as it takes.
    for (;;) {
      if (begin >= hardEnd || measure(begin, hardEnd) <= width) {
        lines.push_back({begin, hardEnd});
        break;
      }
      // The furthest place a line can end: the last break after which the text still fits.
      size_t fit = begin, lastSpace = std::string::npos, at = begin;
      while (at < hardEnd) {
        const unsigned char c = static_cast<unsigned char>(text[at]);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const size_t next = std::min(at + length, hardEnd);
        if (measure(begin, next) > width && fit > begin) break;
        fit = next;
        if (text[at] == ' ') lastSpace = next;
        at = next;
      }
      if (fit == begin) fit = std::min(begin + 1, hardEnd);
      size_t end = lastSpace != std::string::npos && lastSpace > begin && fit < hardEnd ? lastSpace : fit;
      lines.push_back({begin, end});
      begin = end;
    }
    if (newline == std::string::npos) break;
    lineStart = newline + 1;
    if (lineStart > text.size()) break;
  }
  if (lines.empty()) lines.push_back({0, 0});
  return lines;
}

}  // namespace solar::layout
