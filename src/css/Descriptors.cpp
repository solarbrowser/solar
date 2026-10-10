#include "solar/css/Descriptors.h"

#include <cmath>
#include <set>

#include "solar/css/Properties.h"
#include "solar/css/Values.h"

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

ComponentValues Words(const ComponentValues& values) {
  ComponentValues out;
  for (const ComponentValue& v : values) {
    if (!v.IsWhitespace()) out.push_back(v);
  }
  return out;
}

std::optional<std::string> Matched(const char* syntax, const ComponentValues& value) {
  ValueMatch match;
  if (!MatchSyntax(syntax, value, match)) return std::nullopt;
  return SerializeValue(match.normalized);
}

std::optional<std::string> MatchedProperty(const char* property, const ComponentValues& value) {
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition) return std::nullopt;
  ValueMatch match;
  if (!MatchPropertyValue(*definition, value, match)) return std::nullopt;
  return SerializeValue(match.normalized);
}

// ---- src ----

bool KnownFormat(const std::string& keyword) {
  static const std::set<std::string> formats = {"collection", "embedded-opentype", "opentype", "svg", "truetype", "woff", "woff2"};
  return formats.count(keyword) > 0;
}

bool KnownTech(const std::string& keyword) {
  static const std::set<std::string> techs = {"features-opentype", "features-aat", "features-graphite", "color-colrv0", "color-colrv1", "color-svg", "color-sbix", "color-cbdt", "variations", "palettes", "incremental", "incremental-patch", "incremental-range", "incremental-auto"};
  return techs.count(keyword) > 0;
}

// One component of a src list: nothing when it is not a valid one.
std::optional<std::string> FontSource(const ComponentValues& items) {
  if (items.empty()) return std::nullopt;
  const ComponentValue& first = items[0];
  if (first.kind == ComponentValue::Kind::Function && Lower(first.name) == "local") {
    if (items.size() != 1) return std::nullopt;
    const ComponentValues inner = Words(first.children);
    if (inner.empty()) return std::nullopt;
    if (inner.size() == 1 && inner[0].IsToken(T::String)) return "local(" + SerializeString(inner[0].token.value) + ")";
    std::string name;
    for (const ComponentValue& c : inner) {
      if (!c.IsIdent()) return std::nullopt;
      name += (name.empty() ? "" : " ") + SerializeIdentifier(c.token.value);
    }
    if (inner.size() == 1) {
      const std::string lower = Lower(inner[0].token.value);
      if (lower == "default" || IsCssWideKeyword({inner[0]})) return std::nullopt;
    }
    return "local(" + name + ")";
  }
  std::string url;
  if (first.IsToken(T::Url)) {
    url = first.token.value;
  } else if (first.kind == ComponentValue::Kind::Function && Lower(first.name) == "url") {
    const ComponentValues inner = Words(first.children);
    if (inner.size() != 1 || !inner[0].IsToken(T::String)) return std::nullopt;
    url = inner[0].token.value;
  } else {
    return std::nullopt;
  }
  std::string out = "url(" + SerializeString(url) + ")";
  size_t i = 1;
  if (i < items.size() && items[i].kind == ComponentValue::Kind::Function && Lower(items[i].name) == "format") {
    const ComponentValues inner = Words(items[i].children);
    if (inner.size() != 1) return std::nullopt;
    if (inner[0].IsToken(T::String)) out += " format(" + SerializeString(inner[0].token.value) + ")";
    else if (inner[0].IsIdent() && KnownFormat(Lower(inner[0].token.value))) out += " format(" + Lower(inner[0].token.value) + ")";
    else return std::nullopt;
    ++i;
  }
  if (i < items.size() && items[i].kind == ComponentValue::Kind::Function && Lower(items[i].name) == "tech") {
    std::string list;
    const ComponentValues inner = Words(items[i].children);
    if (inner.empty()) return std::nullopt;
    bool expectItem = true;
    for (const ComponentValue& c : inner) {
      if (expectItem) {
        if (!c.IsIdent() || !KnownTech(Lower(c.token.value))) return std::nullopt;
        list += (list.empty() ? "" : ", ") + Lower(c.token.value);
      } else if (!c.IsToken(T::Comma)) {
        return std::nullopt;
      }
      expectItem = !expectItem;
    }
    if (expectItem) return std::nullopt;
    out += " tech(" + list + ")";
    ++i;
  }
  if (i != items.size()) return std::nullopt;
  return out;
}

// A comma-separated list of sources; those that are not valid are dropped, and the list is not valid if none is.
std::optional<std::string> Source(const ComponentValues& value) {
  std::string out;
  for (const ComponentValues& part : SplitOnCommas(value)) {
    if (const std::optional<std::string> one = FontSource(Words(part))) out += (out.empty() ? "" : ", ") + *one;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// ---- unicode-range ----

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::optional<uint32_t> ParseHex(const std::string& text) {
  if (text.empty() || text.size() > 6) return std::nullopt;
  uint32_t n = 0;
  for (char c : text) {
    const int d = HexValue(c);
    if (d < 0) return std::nullopt;
    n = n * 16 + static_cast<uint32_t>(d);
  }
  return n;
}

std::string HexText(uint32_t n) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  do {
    out.insert(out.begin(), digits[n & 15]);
    n >>= 4;
  } while (n);
  return out;
}

// The text of the tokens, with no space between them, as the characters of a <urange>.
std::optional<std::string> Urange(const ComponentValues& value) {
  std::string text;
  for (const ComponentValue& v : value) {
    if (v.kind != ComponentValue::Kind::Token || v.IsWhitespace()) return std::nullopt;
    if (v.token.type == T::Number || v.token.type == T::Dimension) text += v.token.representation + (v.token.type == T::Dimension ? v.token.value : "");
    else if (v.IsDelim('+')) text += "+";
    else if (v.IsDelim('?')) text += "?";
    else if (v.IsIdent()) text += v.token.value;
    else if (v.IsDelim('-')) text += "-";
    else return std::nullopt;
  }
  if (text.size() < 3 || (text[0] != 'u' && text[0] != 'U') || text[1] != '+') return std::nullopt;
  const std::string body = text.substr(2);
  const size_t dash = body.find('-');
  uint32_t low, high;
  if (dash != std::string::npos) {
    const std::optional<uint32_t> a = ParseHex(body.substr(0, dash)), b = ParseHex(body.substr(dash + 1));
    if (!a || !b) return std::nullopt;
    low = *a;
    high = *b;
  } else {
    const size_t question = body.find('?');
    if (question == std::string::npos) {
      const std::optional<uint32_t> a = ParseHex(body);
      if (!a) return std::nullopt;
      low = high = *a;
    } else {
      if (body.size() > 6) return std::nullopt;
      for (size_t i = question; i < body.size(); ++i) {
        if (body[i] != '?') return std::nullopt;
      }
      const std::string digits = body.substr(0, question);
      std::optional<uint32_t> base = digits.empty() ? std::optional<uint32_t>(0) : ParseHex(digits);
      if (!base) return std::nullopt;
      const size_t wild = body.size() - question;
      low = *base << (4 * wild);
      high = low + (1u << (4 * wild)) - 1;
    }
  }
  if (high > 0x10FFFF || low > high) return std::nullopt;
  return "U+" + HexText(low) + (high != low ? "-" + HexText(high) : "");
}

std::optional<std::string> UnicodeRange(const ComponentValues& value) {
  std::string out;
  for (const ComponentValues& part : SplitOnCommas(value)) {
    const std::optional<std::string> one = Urange(Words(part).empty() ? part : part);
    if (!one) return std::nullopt;
    out += (out.empty() ? "" : ", ") + *one;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// ---- font-style ----

std::optional<std::string> FaceStyle(const ComponentValues& value) {
  ValueMatch match;
  if (!MatchSyntax("auto | normal | italic | oblique <angle>{1,2}", value, match)) return std::nullopt;
  for (const ComponentValue& v : match.normalized) {
    if (!v.IsToken(T::Dimension)) continue;
    const std::string unit = Lower(v.token.value);
    double degrees = v.token.number;
    if (unit == "grad") degrees = degrees * 0.9;
    else if (unit == "rad") degrees = degrees * 180 / M_PI;
    else if (unit == "turn") degrees = degrees * 360;
    if (degrees < -90 || degrees > 90) return std::nullopt;
  }
  // oblique 14deg is oblique.
  if (match.normalized.size() == 2 && match.normalized[0].IsIdent() && Lower(match.normalized[0].token.value) == "oblique" && match.normalized[1].IsToken(T::Dimension) && match.normalized[1].token.number == 14 && Lower(match.normalized[1].token.value) == "deg") {
    return std::string("oblique");
  }
  return SerializeValue(match.normalized);
}

// The two ends of a range given twice are one.
std::optional<std::string> Range(const char* syntax, const ComponentValues& value) {
  ValueMatch match;
  if (!MatchSyntax(syntax, value, match)) return std::nullopt;
  if (match.normalized.size() == 2 && SerializeValue({match.normalized[0]}) == SerializeValue({match.normalized[1]})) match.normalized.pop_back();
  return SerializeValue(match.normalized);
}

// Whether a color depends on where it is used: currentcolor, the system colors, light-dark().
bool ContextualColor(const ComponentValues& values) {
  static const std::set<std::string> system = {"currentcolor", "accentcolor", "accentcolortext", "activetext", "buttonborder", "buttonface", "buttontext", "canvas", "canvastext", "field", "fieldtext", "graytext", "highlight", "highlighttext",
                                               "linktext", "mark", "marktext", "selecteditem", "selecteditemtext", "visitedtext", "activeborder", "activecaption", "appworkspace", "background", "buttonhighlight",
                                               "buttonshadow", "captiontext", "inactiveborder", "inactivecaption", "inactivecaptiontext", "infobackground", "infotext", "menu", "menutext", "scrollbar", "threeddarkshadow",
                                               "threedface", "threedhighlight", "threedlightshadow", "threedshadow", "window", "windowframe", "windowtext"};
  for (const ComponentValue& v : values) {
    if (v.IsIdent() && system.count(Lower(v.token.value))) return true;
    if (v.kind == ComponentValue::Kind::Function && Lower(v.name) == "light-dark") return true;
    if (v.kind != ComponentValue::Kind::Token && ContextualColor(v.children)) return true;
  }
  return false;
}

// ---- @counter-style ----

bool CounterStyleNameOk(const std::string& name) {
  const std::string lower = Lower(name);
  return lower != "none" && lower != "initial" && lower != "inherit" && lower != "unset" && lower != "default" && lower != "revert" && lower != "revert-layer";
}

// A <symbol>: a string or an identifier, serialized.
std::optional<std::string> Symbol(const ComponentValue& v) {
  if (v.IsToken(T::String)) return SerializeString(v.token.value);
  if (v.IsIdent() && !IsCssWideKeyword({v}) && Lower(v.token.value) != "default") return SerializeIdentifier(v.token.value);
  if (v.kind == ComponentValue::Kind::Function || v.IsToken(T::Url)) return Matched("<image>", ComponentValues{v});
  return std::nullopt;
}

std::optional<std::string> CounterStyleDescriptor(const std::string& name, const ComponentValues& value) {
  const ComponentValues w = Words(value);
  if (w.empty()) return std::nullopt;
  if (name == "system") {
    if (!w[0].IsIdent()) return std::nullopt;
    const std::string k = Lower(w[0].token.value);
    if (k == "cyclic" || k == "numeric" || k == "alphabetic" || k == "symbolic" || k == "additive") return w.size() == 1 ? std::optional<std::string>(k) : std::nullopt;
    if (k == "fixed") {
      if (w.size() == 1) return std::string("fixed");
      if (w.size() == 2 && w[1].IsToken(T::Number) && w[1].token.isInteger) return "fixed " + std::to_string(static_cast<long long>(w[1].token.number));
      return std::nullopt;
    }
    if (k == "extends") {
      if (w.size() == 2 && w[1].IsIdent() && CounterStyleNameOk(w[1].token.value)) return "extends " + SerializeIdentifier(w[1].token.value);
    }
    return std::nullopt;
  }
  if (name == "negative") {
    if (w.size() > 2) return std::nullopt;
    std::string out;
    for (const ComponentValue& v : w) {
      const std::optional<std::string> s = Symbol(v);
      if (!s) return std::nullopt;
      out += (out.empty() ? "" : " ") + *s;
    }
    return out;
  }
  if (name == "prefix" || name == "suffix") {
    if (w.size() != 1) return std::nullopt;
    return Symbol(w[0]);
  }
  if (name == "symbols") {
    std::string out;
    for (const ComponentValue& v : w) {
      const std::optional<std::string> s = Symbol(v);
      if (!s) return std::nullopt;
      out += (out.empty() ? "" : " ") + *s;
    }
    return out;
  }
  if (name == "fallback") {
    if (w.size() != 1 || !w[0].IsIdent() || !CounterStyleNameOk(w[0].token.value)) return std::nullopt;
    return SerializeIdentifier(w[0].token.value);
  }
  if (name == "pad") {
    if (w.size() != 2) return std::nullopt;
    const ComponentValue* number = w[0].IsToken(T::Number) ? &w[0] : w[1].IsToken(T::Number) ? &w[1] : nullptr;
    const ComponentValue* symbol = number == &w[0] ? &w[1] : &w[0];
    if (!number || !number->token.isInteger || number->token.number < 0) return std::nullopt;
    const std::optional<std::string> s = Symbol(*symbol);
    if (!s) return std::nullopt;
    return std::to_string(static_cast<long long>(number->token.number)) + " " + *s;
  }
  if (name == "range") {
    if (w.size() == 1 && w[0].IsIdent() && Lower(w[0].token.value) == "auto") return std::string("auto");
    std::string out;
    for (const ComponentValues& part : SplitOnCommas(value)) {
      const ComponentValues p = Words(part);
      if (p.size() != 2) return std::nullopt;
      double bounds[2];
      std::string text[2];
      for (int i = 0; i < 2; ++i) {
        if (p[i].IsToken(T::Number) && p[i].token.isInteger) { bounds[i] = p[i].token.number; text[i] = std::to_string(static_cast<long long>(p[i].token.number)); }
        else if (p[i].IsIdent() && Lower(p[i].token.value) == "infinite") { bounds[i] = i == 0 ? -INFINITY : INFINITY; text[i] = "infinite"; }
        else return std::nullopt;
      }
      if (bounds[0] > bounds[1]) return std::nullopt;
      out += (out.empty() ? "" : ", ") + text[0] + " " + text[1];
    }
    return out.empty() ? std::nullopt : std::optional<std::string>(out);
  }
  if (name == "additive-symbols") {
    std::string out;
    double previous = INFINITY;
    for (const ComponentValues& part : SplitOnCommas(value)) {
      const ComponentValues p = Words(part);
      if (p.size() != 2) return std::nullopt;
      const ComponentValue* number = p[0].IsToken(T::Number) ? &p[0] : p[1].IsToken(T::Number) ? &p[1] : nullptr;
      const ComponentValue* symbol = number == &p[0] ? &p[1] : &p[0];
      if (!number || !number->token.isInteger || number->token.number < 0) return std::nullopt;
      if (number->token.number >= previous) return std::nullopt;
      previous = number->token.number;
      const std::optional<std::string> s = Symbol(*symbol);
      if (!s) return std::nullopt;
      out += (out.empty() ? "" : ", ") + std::to_string(static_cast<long long>(number->token.number)) + " " + *s;
    }
    return out.empty() ? std::nullopt : std::optional<std::string>(out);
  }
  if (name == "speak-as") {
    if (w.size() != 1 || !w[0].IsIdent()) return std::nullopt;
    const std::string k = Lower(w[0].token.value);
    if (k == "auto" || k == "bullets" || k == "numbers" || k == "words" || k == "spell-out") return k;
    if (!CounterStyleNameOk(w[0].token.value)) return std::nullopt;
    return SerializeIdentifier(w[0].token.value);
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> DescriptorName(DescriptorSet set, const std::string& given) {
  const std::string name = Lower(given);
  if (set == DescriptorSet::CounterStyle) {
    static const std::set<std::string> names = {"system", "negative", "prefix", "suffix", "range", "pad", "fallback", "symbols", "additive-symbols", "speak-as"};
    if (names.count(name)) return name;
    return std::nullopt;
  }
  if (set == DescriptorSet::FontFace) {
    static const std::set<std::string> names = {"ascent-override", "descent-override", "line-gap-override", "font-display", "font-family", "font-feature-settings", "font-language-override",
                                               "font-named-instance", "font-style", "font-variation-settings", "font-weight", "font-stretch", "size-adjust", "src", "unicode-range"};
    if (name == "font-width") return std::string("font-stretch");
    if (names.count(name)) return name;
    return std::nullopt;
  }
  if (name == "font-family" || name == "base-palette" || name == "override-colors") return name;
  return std::nullopt;
}

bool IsAnyDescriptorName(const std::string& name) {
  return DescriptorName(DescriptorSet::FontFace, name) || DescriptorName(DescriptorSet::FontPaletteValues, name) || DescriptorName(DescriptorSet::CounterStyle, name);
}

std::optional<std::string> DescriptorValue(DescriptorSet set, const std::string& name, const ComponentValues& given) {
  const ComponentValues value = Trimmed(given);
  if (value.empty()) return std::nullopt;
  if (IsCssWideKeyword(value)) return std::nullopt;
  if (set == DescriptorSet::CounterStyle) return CounterStyleDescriptor(name, value);
  if (set == DescriptorSet::FontPaletteValues) {
    if (name == "font-family") {
      // Several families, but not the generic ones.
      const std::optional<std::string> family = MatchedProperty("font-family", value);
      if (!family) return std::nullopt;
      static const std::set<std::string> generic = {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded", "math", "emoji", "fangsong"};
      size_t start = 0;
      while (start <= family->size()) {
        const size_t comma = family->find(", ", start);
        const std::string one = family->substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (generic.count(Lower(one))) return std::nullopt;
        if (comma == std::string::npos) break;
        start = comma + 2;
      }
      return family;
    }
    if (name == "base-palette") return Matched("light | dark | <integer [0,∞]>", value);
    if (ContextualColor(value)) return std::nullopt;
    return Matched("[ <integer [0,∞]> <color> ]#", value);
  }
  if (name == "src") return Source(value);
  if (name == "unicode-range") return UnicodeRange(value);
  if (name == "font-family") {
    const std::optional<std::string> family = MatchedProperty("font-family", value);
    if (!family || family->find(',') != std::string::npos) return std::nullopt;
    return family;
  }
  if (name == "font-feature-settings" || name == "font-variation-settings" || name == "font-language-override") return MatchedProperty(name.c_str(), value);
  if (name == "font-style") return FaceStyle(value);
  if (name == "font-weight") return Range("auto | [ normal | bold | <number [1,1000]> ]{1,2}", value);
  if (name == "font-stretch") {
    return Range("auto | [ normal | ultra-condensed | extra-condensed | condensed | semi-condensed | semi-expanded | expanded | extra-expanded | ultra-expanded | <percentage [0,∞]> ]{1,2}", value);
  }
  if (name == "font-display") return Matched("auto | block | swap | fallback | optional", value);
  if (name == "font-named-instance") return Matched("auto | <string>", value);
  if (name == "size-adjust") return Matched("<percentage [0,∞]>", value);
  // ascent-override, descent-override, line-gap-override
  return Matched("normal | <percentage [0,∞]>", value);
}

}  // namespace solar::css
