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
  return a ? a->value : std::string();
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void CollectText(const dom::Node* node, std::string& out) {
  for (const dom::Node* child = node->firstChild; child; child = child->nextSibling) {
    if (child->IsText()) out += static_cast<const dom::CharacterData*>(child)->data;
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
      for (const dom::Node* child = parent->firstChild; child; child = child->nextSibling) {
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

}  // namespace solar::layout
