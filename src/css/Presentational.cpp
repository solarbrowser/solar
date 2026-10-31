/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/css/Presentational.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace solar::css {

namespace {

bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && Space(s[a])) ++a;
  while (b > a && Space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

const std::string* Attr(const dom::Element* e, const char* name) {
  const dom::Attr* a = e->FindAttribute("", name);
  return a ? &a->value : nullptr;
}

struct Hints {
  std::vector<DeclarationEntry> out;
  void Add(const char* name, std::string value) {
    // (The shorthands are the four longhands: a hint is a declaration of one property at a time.)
    const std::string n = name;
    static const char* const sides[] = {"top", "right", "bottom", "left"};
    if (n == "padding") { for (const char* side : sides) Add((std::string("padding-") + side).c_str(), value); return; }
    if (n == "border-width" || n == "border-style" || n == "border-color") {
      const std::string kind = n.substr(7);
      for (const char* side : sides) Add((std::string("border-") + side + "-" + kind).c_str(), value);
      return;
    }
    DeclarationEntry entry;
    entry.name = name;
    entry.value = std::move(value);
    out.push_back(std::move(entry));
  }
};

// The rules for parsing non-negative integers (a prefix of digits), as a number; false if there is none.
bool ParseNonNegative(const std::string& text, double& value) {
  size_t i = 0;
  while (i < text.size() && Space(text[i])) ++i;
  if (i < text.size() && text[i] == '+') ++i;
  const size_t start = i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  if (i == start) return false;
  value = std::strtod(text.substr(start, i - start).c_str(), nullptr);
  return true;
}

// The rules for parsing dimension values: digits, a fraction, and a percent sign or nothing (a length in pixels).
bool ParseDimension(const std::string& text, std::string& out) {
  size_t i = 0;
  while (i < text.size() && Space(text[i])) ++i;
  const size_t start = i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  if (i == start) return false;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  }
  const std::string number = text.substr(start, i - start);
  out = number + (i < text.size() && text[i] == '%' ? "%" : "px");
  return true;
}

// The rules for parsing a legacy colour value.
bool ParseLegacyColor(const std::string& given, std::string& out) {
  std::string input = Trim(given);
  if (input.empty() || Lower(input) == "transparent") return false;
  const std::string lowered = Lower(input);
  static const char* const names[] = {"aliceblue", "antiquewhite", "aqua", "aquamarine", "azure", "beige", "bisque", "black", "blanchedalmond", "blue", "blueviolet", "brown", "burlywood", "cadetblue", "chartreuse",
      "chocolate", "coral", "cornflowerblue", "cornsilk", "crimson", "cyan", "darkblue", "darkcyan", "darkgoldenrod", "darkgray", "darkgreen", "darkgrey", "darkkhaki", "darkmagenta", "darkolivegreen",
      "darkorange", "darkorchid", "darkred", "darksalmon", "darkseagreen", "darkslateblue", "darkslategray", "darkslategrey", "darkturquoise", "darkviolet", "deeppink", "deepskyblue", "dimgray", "dimgrey",
      "dodgerblue", "firebrick", "floralwhite", "forestgreen", "fuchsia", "gainsboro", "ghostwhite", "gold", "goldenrod", "gray", "green", "greenyellow", "grey", "honeydew", "hotpink", "indianred", "indigo",
      "ivory", "khaki", "lavender", "lavenderblush", "lawngreen", "lemonchiffon", "lightblue", "lightcoral", "lightcyan", "lightgoldenrodyellow", "lightgray", "lightgreen", "lightgrey", "lightpink",
      "lightsalmon", "lightseagreen", "lightskyblue", "lightslategray", "lightslategrey", "lightsteelblue", "lightyellow", "lime", "limegreen", "linen", "magenta", "maroon", "mediumaquamarine", "mediumblue",
      "mediumorchid", "mediumpurple", "mediumseagreen", "mediumslateblue", "mediumspringgreen", "mediumturquoise", "mediumvioletred", "midnightblue", "mintcream", "mistyrose", "moccasin", "navajowhite", "navy",
      "oldlace", "olive", "olivedrab", "orange", "orangered", "orchid", "palegoldenrod", "palegreen", "paleturquoise", "palevioletred", "papayawhip", "peachpuff", "peru", "pink", "plum", "powderblue", "purple",
      "rebeccapurple", "red", "rosybrown", "royalblue", "saddlebrown", "salmon", "sandybrown", "seagreen", "seashell", "sienna", "silver", "skyblue", "slateblue", "slategray", "slategrey", "snow", "springgreen",
      "steelblue", "tan", "teal", "thistle", "tomato", "turquoise", "violet", "wheat", "white", "whitesmoke", "yellow", "yellowgreen"};
  for (const char* name : names) if (lowered == name) { out = lowered; return true; }
  const auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
  if (input.size() == 4 && input[0] == '#' && hex(input[1]) && hex(input[2]) && hex(input[3])) {
    out = std::string("#") + input[1] + input[1] + input[2] + input[2] + input[3] + input[3];
    return true;
  }
  // (Characters outside the basic plane count as two zeros: the bytes of UTF-8 above one are made into zeros.)
  std::string text;
  for (size_t i = 0; i < input.size();) {
    const unsigned char c = static_cast<unsigned char>(input[i]);
    if (c >= 0xF0) { text += "00"; i += 4; }
    else if (c >= 0xE0) { text += '0'; i += 3; }
    else if (c >= 0xC0) { text += '0'; i += 2; }
    else { text += static_cast<char>(c); ++i; }
  }
  if (text.size() > 128) text.resize(128);
  if (!text.empty() && text[0] == '#') text.erase(0, 1);
  for (char& c : text) if (!hex(c)) c = '0';
  while (text.empty() || text.size() % 3 != 0) text += '0';
  size_t length = text.size() / 3;
  std::string parts[3] = {text.substr(0, length), text.substr(length, length), text.substr(2 * length, length)};
  if (length > 8) {
    for (std::string& p : parts) p = p.substr(length - 8);
    length = 8;
  }
  while (length > 2 && parts[0][0] == '0' && parts[1][0] == '0' && parts[2][0] == '0') {
    for (std::string& p : parts) p.erase(0, 1);
    --length;
  }
  if (length > 2) for (std::string& p : parts) p.resize(2);
  out = "#";
  for (const std::string& p : parts) out += p.size() == 1 ? std::string("0") + p : p;
  return true;
}

const dom::Element* ParentElement(const dom::Element* e) {
  const dom::Node* p = dom::ParentOf(e);
  return p && p->IsElement() ? static_cast<const dom::Element*>(p) : nullptr;
}

const dom::Element* AncestorTable(const dom::Element* e) {
  for (const dom::Element* p = ParentElement(e); p; p = ParentElement(p)) if (p->IsHtml("table")) return p;
  return nullptr;
}

}  // namespace

std::vector<DeclarationEntry> PresentationalHints(const dom::Element* element) {
  Hints h;
  if (!element->IsHtml()) return h.out;
  const std::string& tag = element->localName;
  const auto dimension = [&](const char* attr, const char* property) {
    if (const std::string* v = Attr(element, attr)) { std::string out; if (ParseDimension(*v, out)) h.Add(property, out); }
  };
  const auto color = [&](const char* attr, const char* property) {
    if (const std::string* v = Attr(element, attr)) { std::string out; if (ParseLegacyColor(*v, out)) h.Add(property, out); }
  };
  const bool table = tag == "table", cell = tag == "td" || tag == "th", rowGroup = tag == "thead" || tag == "tbody" || tag == "tfoot", row = tag == "tr";

  // Sizes.
  if (table || cell || tag == "col" || tag == "colgroup") dimension("width", "width");
  if (table || cell || row || rowGroup) dimension("height", "height");
  if (tag == "hr") dimension("width", "width");
  if (tag == "img" || tag == "embed" || tag == "object" || tag == "iframe" || tag == "video") {
    // (Those that are replaced are taken by the block that sets width and height by their own rules.)
  }
  if (tag == "img" || tag == "embed" || tag == "object" || tag == "applet" || tag == "marquee") {
    double v;
    if (const std::string* a = Attr(element, "hspace")) if (ParseNonNegative(*a, v)) { const std::string px = std::to_string(static_cast<long>(v)) + "px"; h.Add("margin-left", px); h.Add("margin-right", px); }
    if (const std::string* a = Attr(element, "vspace")) if (ParseNonNegative(*a, v)) { const std::string px = std::to_string(static_cast<long>(v)) + "px"; h.Add("margin-top", px); h.Add("margin-bottom", px); }
  }
  if (tag == "img" || tag == "object" || tag == "input") {
    double v;
    if (const std::string* a = Attr(element, "border")) if (tag != "input" && ParseNonNegative(*a, v)) { const std::string px = std::to_string(static_cast<long>(v)) + "px"; h.Add("border-width", px); h.Add("border-style", "solid"); }
  }

  // Colors and backgrounds.
  if (tag == "body" || table || rowGroup || row || cell || tag == "marquee") color("bgcolor", "background-color");
  if (tag == "body") color("text", "color");
  if (tag == "font") color("color", "color");
  if (tag == "hr") color("color", "border-color");
  if (tag == "body" || table || cell || tag == "thead" || tag == "tbody" || tag == "tfoot" || row) {
    if (const std::string* v = Attr(element, "background")) if (!v->empty()) h.Add("background-image", "url(\"" + *v + "\")");
  }

  // Fonts.
  if (tag == "font") {
    if (const std::string* v = Attr(element, "face")) if (!Trim(*v).empty()) h.Add("font-family", Trim(*v));
    if (const std::string* v = Attr(element, "size")) {
      // The rules for parsing a legacy font size.
      std::string s = Trim(*v);
      if (!s.empty()) {
        char sign = 0;
        size_t i = 0;
        if (s[0] == '+' || s[0] == '-') { sign = s[0]; i = 1; }
        const size_t digits = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i > digits) {
          long value = std::strtol(s.substr(digits, i - digits).c_str(), nullptr, 10);
          if (sign == '+') value = 3 + value; else if (sign == '-') value = 3 - value;
          value = std::clamp(value, 1L, 7L);
          static const char* const keywords[] = {"", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
          h.Add("font-size", keywords[value]);
        }
      }
    }
  }

  // Alignment.
  if (const std::string* a = Attr(element, "align")) {
    const std::string v = Lower(*a);
    if (tag == "div" || tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "caption" || tag == "col" || tag == "colgroup" || rowGroup || row || cell) {
      if (v == "left") h.Add("text-align", "left");
      else if (v == "right") h.Add("text-align", "right");
      else if (v == "center") h.Add("text-align", "center");
      else if (v == "justify") h.Add("text-align", "justify");
    } else if (table) {
      if (v == "left") h.Add("float", "left");
      else if (v == "right") h.Add("float", "right");
      else if (v == "center" || v == "abscenter" || v == "absmiddle") { h.Add("margin-left", "auto"); h.Add("margin-right", "auto"); }
    } else if (tag == "img" || tag == "embed" || tag == "object" || tag == "applet" || tag == "input" || tag == "iframe") {
      if (v == "left") h.Add("float", "left");
      else if (v == "right") h.Add("float", "right");
      else if (v == "top") h.Add("vertical-align", "top");
      else if (v == "bottom" || v == "baseline") h.Add("vertical-align", "baseline");
      else if (v == "middle" || v == "absmiddle" || v == "abscenter") h.Add("vertical-align", "middle");
    } else if (tag == "hr") {
      if (v == "left") { h.Add("margin-left", "0"); h.Add("margin-right", "auto"); }
      else if (v == "right") { h.Add("margin-left", "auto"); h.Add("margin-right", "0"); }
      else if (v == "center") { h.Add("margin-left", "auto"); h.Add("margin-right", "auto"); }
    }
  }
  if (const std::string* a = Attr(element, "valign")) {
    const std::string v = Lower(*a);
    if (tag == "col" || tag == "colgroup" || rowGroup || row || cell) {
      if (v == "top" || v == "middle" || v == "bottom" || v == "baseline") h.Add("vertical-align", v);
    }
  }
  if (cell && Attr(element, "nowrap") && !Attr(element, "width")) h.Add("white-space", "nowrap");

  // Tables.
  if (table) {
    if (const std::string* a = Attr(element, "cellspacing")) { double v; if (ParseNonNegative(*a, v)) h.Add("border-spacing", std::to_string(static_cast<long>(v)) + "px"); }
    if (const std::string* a = Attr(element, "border")) {
      double v = 1;
      const bool ok = Trim(*a).empty() ? true : ParseNonNegative(*a, v);
      if (ok) { const std::string px = std::to_string(static_cast<long>(v)) + "px"; h.Add("border-width", px); h.Add("border-style", v == 0 ? "none" : "outset"); }
    }
    color("bordercolor", "border-color");
  }
  if (cell) {
    if (const dom::Element* t = AncestorTable(element)) {
      if (const std::string* a = Attr(t, "cellpadding")) { double v; if (ParseNonNegative(*a, v)) h.Add("padding", std::to_string(static_cast<long>(v)) + "px"); }
      if (const std::string* b = Attr(t, "border")) {
        double v = 1;
        const bool ok = Trim(*b).empty() ? true : ParseNonNegative(*b, v);
        if (ok && v > 0) { h.Add("border-width", "1px"); h.Add("border-style", "inset"); if (const std::string* c = Attr(t, "bordercolor")) { std::string out; if (ParseLegacyColor(*c, out)) h.Add("border-color", out); } }
      }
    }
  }

  // Others.
  if (tag == "hr") {
    if (Attr(element, "noshade")) { h.Add("border-style", "solid"); h.Add("border-width", "1px"); h.Add("background-color", "gray"); }
    if (const std::string* a = Attr(element, "size")) { double v; if (ParseNonNegative(*a, v)) h.Add("height", std::to_string(std::max(0L, static_cast<long>(v) - 2)) + "px"); }
  }
  if (tag == "br") {
    if (const std::string* a = Attr(element, "clear")) { const std::string v = Lower(*a); if (v == "left" || v == "right" || v == "all") h.Add("clear", v == "all" ? "both" : v); }
  }
  if (tag == "ul" || tag == "ol" || tag == "li") {
    if (const std::string* a = Attr(element, "type")) {
      const std::string t = *a;
      if (t == "1") h.Add("list-style-type", "decimal");
      else if (t == "a") h.Add("list-style-type", "lower-alpha");
      else if (t == "A") h.Add("list-style-type", "upper-alpha");
      else if (t == "i") h.Add("list-style-type", "lower-roman");
      else if (t == "I") h.Add("list-style-type", "upper-roman");
      else { const std::string l = Lower(t); if (l == "disc" || l == "circle" || l == "square") h.Add("list-style-type", l); }
    }
  }
  if (tag == "body") {
    double v;
    for (const char* name : {"leftmargin", "marginwidth"}) if (const std::string* a = Attr(element, name)) if (ParseNonNegative(*a, v)) { h.Add("margin-left", std::to_string(static_cast<long>(v)) + "px"); break; }
    for (const char* name : {"rightmargin", "marginwidth"}) if (const std::string* a = Attr(element, name)) if (ParseNonNegative(*a, v)) { h.Add("margin-right", std::to_string(static_cast<long>(v)) + "px"); break; }
    for (const char* name : {"topmargin", "marginheight"}) if (const std::string* a = Attr(element, name)) if (ParseNonNegative(*a, v)) { h.Add("margin-top", std::to_string(static_cast<long>(v)) + "px"); break; }
    for (const char* name : {"bottommargin", "marginheight"}) if (const std::string* a = Attr(element, name)) if (ParseNonNegative(*a, v)) { h.Add("margin-bottom", std::to_string(static_cast<long>(v)) + "px"); break; }
  }
  return h.out;
}

}  // namespace solar::css
