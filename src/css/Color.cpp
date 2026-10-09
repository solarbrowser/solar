#include "solar/css/Color.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "solar/css/Calc.h"
#include "solar/css/Values.h"

namespace solar::css {

namespace {

using T = Token::Type;
using Cv = ComponentValue;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// ---- Component value builders ----

// Six significant digits, as the numbers of a color are written.
double Round6(double x) {
  if (!std::isfinite(x) || x == 0) return x == 0 ? 0 : x;
  const double magnitude = std::floor(std::log10(std::fabs(x)));
  const double scale = std::pow(10.0, 5 - magnitude);
  const double r = std::round(x * scale) / scale;
  return r == 0 ? 0 : r;
}
double Round8(double x) {
  if (!std::isfinite(x)) return x;
  const double r = std::round(x * 1e8) / 1e8;
  return r == 0 ? 0 : r;
}

Cv Num(double v) {
  Cv c;
  c.token.type = T::Number;
  c.token.number = Round6(v);
  c.token.isInteger = c.token.number == std::floor(c.token.number);
  return c;
}
Cv Pct(double v) {
  Cv c;
  c.token.type = T::Percentage;
  c.token.number = Round6(v);
  return c;
}
Cv NumFine(double v) {
  Cv c = Num(0);
  c.token.number = Round8(v);
  return c;
}
Cv Ident(const std::string& name) {
  Cv c;
  c.token.type = T::Ident;
  c.token.value = name;
  return c;
}
Cv Comma() {
  Cv c;
  c.token.type = T::Comma;
  return c;
}
Cv Slash() {
  Cv c;
  c.token.type = T::Delim;
  c.token.delim = '/';
  return c;
}
Cv Fn(const std::string& name, ComponentValues children) {
  Cv c;
  c.kind = Cv::Kind::Function;
  c.name = name;
  c.children = std::move(children);
  return c;
}

// ---- Named colors ----

struct Named {
  const char* name;
  unsigned rgb;
};

const Named kNamedColors[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff}, {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e}, {"coral", 0xff7f50},
    {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c}, {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080},
    {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee},
    {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

const std::set<std::string>& KeywordColors() {
  static const std::set<std::string> keywords = {
      "currentcolor", "transparent",
      // system colors
      "accentcolor", "accentcolortext", "activetext", "buttonborder", "buttonface", "buttontext", "canvas", "canvastext", "field", "fieldtext", "graytext",
      "highlight", "highlighttext", "linktext", "mark", "marktext", "selecteditem", "selecteditemtext", "visitedtext",
      // deprecated system colors
      "activeborder", "activecaption", "appworkspace", "background", "buttonhighlight", "buttonshadow", "captiontext", "inactiveborder", "inactivecaption",
      "inactivecaptiontext", "infobackground", "infotext", "menu", "menutext", "scrollbar", "threeddarkshadow", "threedface", "threedhighlight",
      "threedlightshadow", "threedshadow", "window", "windowframe", "windowtext"};
  return keywords;
}

// ---- Channels ----

struct Chan {
  enum class Kind { Value, None, Calc };
  Kind kind = Kind::Value;
  double value = 0;
  bool evaluated = true;  // a Calc channel whose value is known
  Cv calc;                // the simplified math function, for Calc
  bool plain = false;     // written as a number, percentage or angle token, not a function
};

// A channel from its component value. `percentScale` is what 100% is; an angle (only for `angle`) is in degrees.
// `names` are the channel keywords a relative color may use in a calculation.
bool ParseChan(const Cv& v, double percentScale, bool angle, const std::vector<std::string>* names, Chan& out) {
  if (v.IsIdent() && Lower(v.token.value) == "none") {
    out.kind = Chan::Kind::None;
    return true;
  }
  if (v.kind == Cv::Kind::Token) {
    if (v.token.type == T::Number) {
      out.value = v.token.number;
      out.plain = true;
      return true;
    }
    if (v.token.type == T::Percentage && !angle) {
      out.value = v.token.number * percentScale / 100;
      out.plain = true;
      return true;
    }
    if (angle && v.token.type == T::Dimension) {
      const std::optional<MathValue> m = EvaluateNumeric(v);
      if (!m || m->kind != MathKind::Angle) return false;
      out.value = m->value;
      out.plain = true;
      return true;
    }
    return false;
  }
  if (v.kind == Cv::Kind::Function && IsMathFunctionName(v.name)) {
    if (!names && !IsValidMathFunction(v)) return false;
    std::optional<ComponentValue> normalized = NormalizeMathFunction(v, names);
    if (!normalized) return false;
    out.kind = Chan::Kind::Calc;
    out.calc = *normalized;
    const std::optional<MathValue> m = EvaluateNumeric(v);
    if (m) {
      const bool okKind = m->kind == MathKind::Number || (!angle && m->kind == MathKind::Percentage) || (angle && m->kind == MathKind::Angle);
      if (!okKind) return false;
      out.value = m->kind == MathKind::Percentage ? m->value * percentScale / 100 : m->value;
      out.evaluated = true;
    } else {
      out.evaluated = false;
    }
    return true;
  }
  return false;
}

double Clamp(double v, double lo, double hi) { return std::isnan(v) ? lo : std::max(lo, std::min(v, hi)); }

// Alpha as the legacy functions write it: the shortest of two or three digits that is the same to eight bits.
std::string AlphaText(double alpha) {
  const double a = Clamp(alpha, 0, 1);
  const long eight = std::lround(a * 255);
  double v = std::round(a * 100) / 100;
  if (std::lround(v * 255) != eight) v = std::round(a * 1000) / 1000;
  return FormatNumber(v);
}

// ---- Spaces ----

double HueToRgb(double t1, double t2, double hue) {
  if (hue < 0) hue += 6;
  if (hue >= 6) hue -= 6;
  if (hue < 1) return (t2 - t1) * hue + t1;
  if (hue < 3) return t2;
  if (hue < 4) return (t2 - t1) * (4 - hue) + t1;
  return t1;
}

void HslToRgb(double h, double s, double l, double rgb[3]) {
  if (!std::isfinite(h)) h = 0;
  h = std::fmod(h, 360);
  if (h < 0) h += 360;
  h /= 60;
  s = Clamp(s, 0, 100) / 100;
  l = Clamp(l, 0, 100) / 100;
  const double t2 = l <= 0.5 ? l * (s + 1) : l + s - l * s;
  const double t1 = l * 2 - t2;
  rgb[0] = HueToRgb(t1, t2, h + 2);
  rgb[1] = HueToRgb(t1, t2, h);
  rgb[2] = HueToRgb(t1, t2, h - 2);
}

void HwbToRgb(double h, double w, double b, double rgb[3]) {
  w = Clamp(w, 0, 100) / 100;
  b = Clamp(b, 0, 100) / 100;
  if (w + b >= 1) {
    const double gray = w / (w + b);
    rgb[0] = rgb[1] = rgb[2] = gray;
    return;
  }
  HslToRgb(h, 100, 50, rgb);
  for (int i = 0; i < 3; ++i) rgb[i] = rgb[i] * (1 - w - b) + w;
}

double NormalizeHue(double h) {
  if (!std::isfinite(h)) return 0;
  h = std::fmod(h, 360);
  return h < 0 ? h + 360 : h;
}

// ---- Output ----

Cv LegacyRgb(const double rgb[3], double alpha) {
  const auto byte = [](double v) { return Num(std::floor(Clamp(v, 0, 1) * 255 + 0.5 + 1e-7)); };
  const double a = Clamp(alpha, 0, 1);
  ComponentValues parts{byte(rgb[0]), Comma(), byte(rgb[1]), Comma(), byte(rgb[2])};
  if (a != 1) {
    parts.push_back(Comma());
    Cv alphaCv = Num(0);
    alphaCv.token.number = std::stod(AlphaText(a));
    parts.push_back(alphaCv);
    return Fn("rgba", parts);
  }
  return Fn("rgb", parts);
}

void AppendAlpha(const Chan& alpha, ComponentValues& out) {
  if (alpha.kind == Chan::Kind::None) {
    out.push_back(Slash());
    out.push_back(Ident("none"));
  } else if (alpha.kind == Chan::Kind::Calc) {
    out.push_back(Slash());
    out.push_back(alpha.calc);
  } else {
    const double a = Clamp(alpha.value, 0, 1);
    if (a == 1) return;
    out.push_back(Slash());
    out.push_back(Num(a));
  }
}

// The channel as a number in the output; Calc channels as the function they are.
Cv ChanCv(const Chan& c, double scale = 1, double lo = -HUGE_VAL, double hi = HUGE_VAL, bool percent = false) {
  if (c.kind == Chan::Kind::None) return Ident("none");
  if (c.kind == Chan::Kind::Calc) return c.calc;
  const double v = Clamp(c.value, lo, hi) / scale;
  return percent ? Pct(v) : Num(v);
}

// ---- Parsing the functions ----

struct Parts {
  ComponentValues channels;  // before the /
  ComponentValues alpha;     // after it
  bool slash = false;
  bool commas = false;
};

bool SplitParts(const ComponentValues& children, Parts& out) {
  bool afterSlash = false;
  for (const Cv& v : children) {
    if (v.IsWhitespace()) continue;
    if (v.IsDelim('/')) {
      if (afterSlash) return false;
      afterSlash = true;
      out.slash = true;
      continue;
    }
    if (v.IsToken(T::Comma)) {
      out.commas = true;
      continue;
    }
    (afterSlash ? out.alpha : out.channels).push_back(v);
  }
  return true;
}

const std::vector<std::string>& ChannelNames(const std::string& fn, const std::string& space) {
  static const std::map<std::string, std::vector<std::string>> names = {
      {"rgb", {"r", "g", "b", "alpha"}}, {"hsl", {"h", "s", "l", "alpha"}}, {"hwb", {"h", "w", "b", "alpha"}},
      {"lab", {"l", "a", "b", "alpha"}}, {"lch", {"l", "c", "h", "alpha"}}, {"oklab", {"l", "a", "b", "alpha"}},
      {"oklch", {"l", "c", "h", "alpha"}}, {"xyz", {"x", "y", "z", "alpha"}}, {"srgb", {"r", "g", "b", "alpha"}}};
  const auto found = names.find(fn == "color" ? (space.starts_with("xyz") ? "xyz" : "srgb") : fn);
  static const std::vector<std::string> none;
  return found == names.end() ? none : found->second;
}

std::optional<ComponentValue> NormalizeInner(const Cv& v);

// The expression with the channel keywords replaced by a number of the type each has (an angle for the hue), to ask what
// type the whole is.
Cv WithKeywordsAsNumbers(const Cv& v, const std::vector<std::string>& names, const std::string& hueName) {
  if (v.kind == Cv::Kind::Token) {
    if (v.IsIdent()) {
      const std::string word = Lower(v.token.value);
      if (std::find(names.begin(), names.end(), word) != names.end()) {
        Cv c;
        (void)hueName;
        c.token.type = T::Number;
        return c;
      }
    }
    return v;
  }
  Cv copy = v;
  for (Cv& child : copy.children) child = WithKeywordsAsNumbers(child, names, hueName);
  return copy;
}

// A relative color: from <color> and then the channels and the alpha, in numbers, percentages, none, channel keywords or
// calculations.
std::optional<Cv> RelativeColor(const std::string& fn, const std::string& outName, const ComponentValues& items, const std::string& space) {
  // items[0] is `from`.
  if (items.size() < 2) return std::nullopt;
  std::optional<Cv> base = NormalizeInner(items[1]);
  if (!base) return std::nullopt;
  const std::vector<std::string>& names = ChannelNames(fn, space);
  const int hueAt = (fn == "hsl" || fn == "hwb") ? 0 : (fn == "lch" || fn == "oklch") ? 2 : -1;
  const std::string hueName = hueAt >= 0 ? "h" : "";
  ComponentValues out{Ident("from"), *base};
  size_t channels = 0;
  bool slash = false;
  size_t alphaCount = 0;
  if (!space.empty()) out.push_back(Ident(space));
  for (size_t i = 2; i < items.size(); ++i) {
    const Cv& v = items[i];
    if (v.IsWhitespace()) continue;
    if (v.IsDelim('/')) {
      if (slash) return std::nullopt;
      slash = true;
      out.push_back(Slash());
      continue;
    }
    const int position = slash ? 3 : static_cast<int>(channels);
    if (slash) ++alphaCount; else ++channels;
    const bool angleHere = position == hueAt;
    if (v.IsIdent()) {
      const std::string word = Lower(v.token.value);
      if (word != "none" && std::find(names.begin(), names.end(), word) == names.end()) return std::nullopt;
      out.push_back(Ident(word));
    } else if (v.kind == Cv::Kind::Token && v.token.type == T::Number) {
      out.push_back(Num(v.token.number));
    } else if (v.kind == Cv::Kind::Token && v.token.type == T::Percentage) {
      if (angleHere) return std::nullopt;
      out.push_back(v);
    } else if (v.kind == Cv::Kind::Token && v.token.type == T::Dimension) {
      const std::optional<MathValue> m = EvaluateNumeric(v);
      if (!angleHere || !m || m->kind != MathKind::Angle) return std::nullopt;
      out.push_back(v);
    } else if (v.kind == Cv::Kind::Function && IsMathFunctionName(v.name)) {
      const Cv typed = WithKeywordsAsNumbers(v, names, hueName);
      if (!IsValidMathFunction(typed)) return std::nullopt;
      if (const std::optional<MathValue> m = EvaluateNumeric(typed)) {
        const bool ok = m->kind == MathKind::Number || (m->kind == MathKind::Percentage && !angleHere) || (m->kind == MathKind::Angle && angleHere);
        if (!ok) return std::nullopt;
      }
      std::optional<Cv> normalized = NormalizeMathFunction(v, &names);
      if (!normalized) return std::nullopt;
      out.push_back(*normalized);
    } else {
      return std::nullopt;
    }
  }
  if (channels != 3 || (slash && alphaCount != 1)) return std::nullopt;
  return Fn(outName, out);
}

std::optional<Cv> ParseRgbLike(const std::string& name, const ComponentValues& children, bool isRgb, bool isHsl) {
  Parts parts;
  if (!SplitParts(children, parts)) return std::nullopt;
  if (!parts.channels.empty() && parts.channels[0].IsIdent() && Lower(parts.channels[0].token.value) == "from") {
    if (parts.commas) return std::nullopt;
    ComponentValues items = parts.channels;
    if (parts.slash) {
      items.push_back(Slash());
      for (const Cv& a : parts.alpha) items.push_back(a);
    }
    return RelativeColor(isRgb ? "rgb" : isHsl ? "hsl" : "hwb", isRgb ? "rgb" : isHsl ? "hsl" : "hwb", items, "");
  }
  const bool legacySyntax = parts.commas;
  if (legacySyntax) {
    // rgb(r, g, b, a?): the values separated by commas, the last the alpha, and no /. hwb() has no legacy syntax.
    if (parts.slash || (!isRgb && !isHsl)) return std::nullopt;
    if (parts.channels.size() != 3 && parts.channels.size() != 4) return std::nullopt;
    size_t commaCount = 0;
    for (const Cv& v : children) {
      if (v.IsToken(T::Comma)) ++commaCount;
    }
    if (commaCount != parts.channels.size() - 1) return std::nullopt;
    // Between two values there is exactly one comma: the SplitParts lost where they were, so check on the original.
    {
      bool expectValue = true;
      for (const Cv& v : children) {
        if (v.IsWhitespace()) continue;
        if (v.IsToken(T::Comma)) {
          if (expectValue) return std::nullopt;
          expectValue = true;
        } else {
          if (!expectValue) return std::nullopt;
          expectValue = false;
        }
      }
      if (expectValue) return std::nullopt;
    }
    if (parts.channels.size() == 4) {
      parts.alpha.push_back(parts.channels[3]);
      parts.channels.pop_back();
      parts.slash = true;
    }
  } else if (parts.channels.size() != 3 || (parts.slash && parts.alpha.size() != 1)) {
    return std::nullopt;
  }
  Chan c[3], alpha;
  alpha.value = 1;
  const double scales[3] = {isRgb ? 255.0 : 360.0, 100.0, 100.0};
  for (int i = 0; i < 3; ++i) {
    const bool hue = !isRgb && i == 0;
    if (!ParseChan(parts.channels[i], isRgb ? 255 : 100, hue, nullptr, c[i])) return std::nullopt;
  }
  (void)scales;
  if (parts.slash && !ParseChan(parts.alpha[0], 1, false, nullptr, alpha)) return std::nullopt;
  // Legacy syntax: no none, and for rgb() all numbers or all percentages; for hsl() percentages for s and l.
  if (legacySyntax) {
    for (int i = 0; i < 3; ++i) {
      if (c[i].kind == Chan::Kind::None) return std::nullopt;
    }
    if (alpha.kind == Chan::Kind::None) return std::nullopt;
    if (isRgb) {
      const bool p0 = parts.channels[0].kind == Cv::Kind::Token && parts.channels[0].token.type == T::Percentage;
      for (int i = 1; i < 3; ++i) {
        const bool p = parts.channels[i].kind == Cv::Kind::Token && parts.channels[i].token.type == T::Percentage;
        const bool number = parts.channels[i].kind == Cv::Kind::Token && parts.channels[i].token.type == T::Number;
        const bool p0number = parts.channels[0].kind == Cv::Kind::Token && parts.channels[0].token.type == T::Number;
        if ((p && p0number) || (number && p0)) return std::nullopt;
      }
    } else {
      for (int i = 1; i < 3; ++i) {
        if (parts.channels[i].kind == Cv::Kind::Token && parts.channels[i].token.type == T::Number) return std::nullopt;
      }
      if (parts.channels[0].kind == Cv::Kind::Token && parts.channels[0].token.type == T::Percentage) return std::nullopt;
    }
  }
  bool anyNone = alpha.kind == Chan::Kind::None, anySymbolic = false;
  for (int i = 0; i < 3; ++i) {
    anyNone = anyNone || c[i].kind == Chan::Kind::None;
    anySymbolic = anySymbolic || (c[i].kind == Chan::Kind::Calc && !c[i].evaluated);
  }
  anySymbolic = anySymbolic || (alpha.kind == Chan::Kind::Calc && !alpha.evaluated);
  const auto numberOrZero = [](double v) { return std::isnan(v) ? 0.0 : v; };

  if (!anyNone && !anySymbolic) {
    // Legacy form: rgb()/rgba() of the resolved sRGB color.
    double rgb[3];
    if (isRgb) {
      for (int i = 0; i < 3; ++i) rgb[i] = Clamp(numberOrZero(c[i].value), 0, 255) / 255;
    } else if (isHsl) {
      HslToRgb(numberOrZero(c[0].value), numberOrZero(c[1].value), numberOrZero(c[2].value), rgb);
    } else {
      HwbToRgb(numberOrZero(c[0].value), numberOrZero(c[1].value), numberOrZero(c[2].value), rgb);
    }
    return LegacyRgb(rgb, numberOrZero(alpha.value));
  }
  if (anySymbolic) {
    // The modern form with what is known written out and the rest as calculations.
    ComponentValues out;
    for (int i = 0; i < 3; ++i) {
      if (isRgb) out.push_back(ChanCv(c[i], 1, 0, 255));
      else if (i == 0) out.push_back(c[0].kind == Chan::Kind::Calc ? c[0].calc : (c[0].kind == Chan::Kind::None ? Ident("none") : Num(NormalizeHue(c[0].value))));
      else out.push_back(ChanCv(c[i], 1, 0, HUGE_VAL));
    }
    AppendAlpha(alpha, out);
    return Fn(isRgb ? "rgb" : isHsl ? "hsl" : "hwb", out);
  }
  // None somewhere.
  if (isRgb) {
    ComponentValues out{Ident("srgb")};
    for (int i = 0; i < 3; ++i) out.push_back(c[i].kind == Chan::Kind::None ? Ident("none") : c[i].kind == Chan::Kind::Calc ? c[i].calc : NumFine(Clamp(c[i].value, 0, 255) / 255));
    AppendAlpha(alpha, out);
    return Fn("color", out);
  }
  ComponentValues out;
  out.push_back(c[0].kind == Chan::Kind::None ? Ident("none") : c[0].kind == Chan::Kind::Calc ? c[0].calc : Num(NormalizeHue(c[0].value)));
  for (int i = 1; i < 3; ++i) out.push_back(c[i].kind == Chan::Kind::None ? Ident("none") : c[i].kind == Chan::Kind::Calc ? c[i].calc : Pct(Clamp(c[i].value, 0, HUGE_VAL)));
  AppendAlpha(alpha, out);
  return Fn(isHsl ? "hsl" : "hwb", out);
}

// lab(), lch(), oklab(), oklch().
std::optional<Cv> ParseLabLike(const std::string& name, const ComponentValues& children) {
  Parts parts;
  if (!SplitParts(children, parts) || parts.commas) return std::nullopt;
  if (!parts.channels.empty() && parts.channels[0].IsIdent() && Lower(parts.channels[0].token.value) == "from") {
    ComponentValues items = parts.channels;
    if (parts.slash) {
      items.push_back(Slash());
      for (const Cv& a : parts.alpha) items.push_back(a);
    }
    return RelativeColor(name, name, items, "");
  }
  if (parts.channels.size() != 3 || (parts.slash && parts.alpha.size() != 1)) return std::nullopt;
  const bool ok = name == "oklab" || name == "oklch";
  const bool polar = name == "lch" || name == "oklch";
  // 100% of L, of a and b (or C).
  const double lScale = ok ? 1 : 100;
  const double abScale = polar ? (ok ? 0.4 : 150) : (ok ? 0.4 : 125);
  Chan c[3], alpha;
  alpha.value = 1;
  if (!ParseChan(parts.channels[0], lScale, false, nullptr, c[0])) return std::nullopt;
  if (!ParseChan(parts.channels[1], abScale, false, nullptr, c[1])) return std::nullopt;
  if (!ParseChan(parts.channels[2], polar ? 360 : abScale, polar, nullptr, c[2])) return std::nullopt;
  if (parts.slash && !ParseChan(parts.alpha[0], 1, false, nullptr, alpha)) return std::nullopt;
  ComponentValues out;
  out.push_back(ChanCv(c[0], 1, 0, lScale));
  out.push_back(polar ? ChanCv(c[1], 1, 0, HUGE_VAL) : ChanCv(c[1]));
  out.push_back(!polar ? ChanCv(c[2]) : c[2].kind == Chan::Kind::None ? Ident("none") : c[2].kind == Chan::Kind::Calc ? c[2].calc : Num(NormalizeHue(c[2].value)));
  AppendAlpha(alpha, out);
  return Fn(name, out);
}

std::optional<Cv> ParseColorFunction(const ComponentValues& children) {
  Parts parts;
  if (!SplitParts(children, parts) || parts.commas) return std::nullopt;
  if (parts.channels.empty()) return std::nullopt;
  size_t at = 0;
  bool relative = false;
  if (parts.channels[0].IsIdent() && Lower(parts.channels[0].token.value) == "from") {
    relative = true;
    at = 2;
  }
  if (parts.channels.size() <= at) return std::nullopt;
  if (!parts.channels[at].IsIdent()) return std::nullopt;
  std::string space = Lower(parts.channels[at].token.value);
  static const std::set<std::string> spaces = {"srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020", "xyz", "xyz-d50", "xyz-d65"};
  if (!spaces.count(space)) return std::nullopt;
  if (relative) {
    // from <color> <space> channels...
    ComponentValues items{parts.channels[0], parts.channels[1]};
    for (size_t i = 3; i < parts.channels.size(); ++i) items.push_back(parts.channels[i]);
    if (parts.slash) {
      items.push_back(Slash());
      for (const Cv& a : parts.alpha) items.push_back(a);
    }
    if (space == "xyz") space = "xyz-d65";
    std::optional<Cv> r = RelativeColor("color", "color", items, space);
    return r;
  }
  if (parts.channels.size() != 4 || (parts.slash && parts.alpha.size() != 1)) return std::nullopt;
  if (space == "xyz") space = "xyz-d65";
  Chan c[3], alpha;
  alpha.value = 1;
  for (int i = 0; i < 3; ++i) {
    if (!ParseChan(parts.channels[i + 1], 1, false, nullptr, c[i])) return std::nullopt;
  }
  if (parts.slash && !ParseChan(parts.alpha[0], 1, false, nullptr, alpha)) return std::nullopt;
  ComponentValues out{Ident(space)};
  for (int i = 0; i < 3; ++i) out.push_back(c[i].kind == Chan::Kind::Value ? NumFine(c[i].value) : ChanCv(c[i]));
  AppendAlpha(alpha, out);
  return Fn("color", out);
}

std::optional<Cv> ParseHex(const std::string& digits) {
  if (!(digits.size() == 3 || digits.size() == 4 || digits.size() == 6 || digits.size() == 8)) return std::nullopt;
  for (char c : digits) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
  }
  const auto hex = [&](size_t at, size_t len) {
    unsigned v = static_cast<unsigned>(std::stoul(digits.substr(at, len), nullptr, 16));
    return len == 1 ? v * 17 : v;
  };
  const size_t len = digits.size() <= 4 ? 1 : 2;
  const double rgb[3] = {hex(0, len) / 255.0, hex(len, len) / 255.0, hex(2 * len, len) / 255.0};
  const double alpha = (digits.size() == 4 || digits.size() == 8) ? hex(3 * len, len) / 255.0 : 1.0;
  return LegacyRgb(rgb, alpha);
}

std::optional<Cv> NormalizeColorMix(const ComponentValues& children) {
  // [in <space> [<hue> hue]?,]? <color> <pct>?, <color> <pct>?
  std::vector<ComponentValues> parts = SplitOnCommas(children);
  std::string space = "oklab";
  ComponentValues out;
  bool explicitMethod = false;
  const ComponentValues method = Trimmed(parts[0]);
  if (!method.empty() && method[0].IsIdent() && Lower(method[0].token.value) == "in") {
    explicitMethod = true;
    std::vector<Cv> words;
    for (const Cv& v : method) {
      if (!v.IsWhitespace()) words.push_back(v);
    }
    if (words.size() < 2 || !words[1].IsIdent()) return std::nullopt;
    space = Lower(words[1].token.value);
    static const std::set<std::string> rectangular = {"srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020", "lab", "oklab", "xyz", "xyz-d50", "xyz-d65"};
    static const std::set<std::string> polar = {"hsl", "hwb", "lch", "oklch"};
    if (space == "xyz") space = "xyz-d65";
    if (rectangular.count(space)) {
      if (words.size() != 2) return std::nullopt;
    } else if (polar.count(space)) {
      if (words.size() == 4) {
        const std::string how = Lower(words[2].token.value);
        if (!words[2].IsIdent() || !words[3].IsIdent() || Lower(words[3].token.value) != "hue") return std::nullopt;
        if (how != "shorter" && how != "longer" && how != "increasing" && how != "decreasing") return std::nullopt;
        if (how != "shorter") {
          out.push_back(Ident(how));
          out.push_back(Ident("hue"));
        }
      } else if (words.size() != 2) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
    parts.erase(parts.begin());
  }
  if (parts.empty()) return std::nullopt;
  // The interpolation method is left out when it is the default.
  ComponentValues head;
  if (explicitMethod && !(space == "oklab" && out.empty())) {
    head.push_back(Ident("in"));
    head.push_back(Ident(space));
    for (const Cv& v : out) head.push_back(v);
  }
  out = head;
  struct Item {
    Cv color;
    std::optional<Cv> percent;
    std::optional<double> number;  // its percentage, when it is a plain one
  };
  std::vector<Item> items(parts.size());
  bool symbolic = false;
  for (size_t i = 0; i < parts.size(); ++i) {
    ComponentValues item = Trimmed(parts[i]);
    std::vector<Cv> pieces;
    for (const Cv& v : item) {
      if (!v.IsWhitespace()) pieces.push_back(v);
    }
    if (pieces.empty() || pieces.size() > 2) return std::nullopt;
    const auto isPercent = [](const Cv& v) {
      return (v.kind == Cv::Kind::Token && v.token.type == T::Percentage) || (v.kind == Cv::Kind::Function && IsMathFunctionName(v.name));
    };
    size_t colorAt = 0;
    if (pieces.size() == 2) {
      if (isPercent(pieces[0]) && !isPercent(pieces[1])) colorAt = 1;
      else if (isPercent(pieces[1]) && !isPercent(pieces[0])) colorAt = 0;
      else return std::nullopt;
      const Cv& pct = pieces[1 - colorAt];
      if (pct.kind == Cv::Kind::Token) {
        if (pct.token.number < 0 || pct.token.number > 100) return std::nullopt;
        items[i].percent = Pct(pct.token.number);
        items[i].number = pct.token.number;
      } else {
        std::optional<Cv> normalized = NormalizeMathFunction(pct);
        if (!normalized) return std::nullopt;
        items[i].percent = *normalized;
        symbolic = true;
      }
    }
    std::optional<Cv> color = NormalizeInner(pieces[colorAt]);
    if (!color) return std::nullopt;
    items[i].color = *color;
  }
  // The percentages left out share what the others leave of 100%; one that is what it would be anyway is not written.
  std::vector<bool> write(items.size(), true);
  if (!symbolic) {
    double given = 0;
    size_t omitted = 0, present = 0;
    for (const Item& item : items) {
      if (item.number) { given += *item.number; ++present; } else ++omitted;
    }
    if (present == 0) {
      for (size_t i = 0; i < items.size(); ++i) write[i] = false;
    } else if (omitted > 0) {
      const double share = std::max(0.0, 100 - given) / static_cast<double>(omitted);
      for (Item& item : items) {
        if (!item.number) {
          item.number = share;
          item.percent = Pct(share);
        }
      }
    }
    if (items.size() == 2 && *items[0].number == 50 && *items[1].number == 50) write[0] = write[1] = false;
    if (items.size() == 1 && *items[0].number == 100) write[0] = false;
  } else {
    for (size_t i = 0; i < items.size(); ++i) write[i] = items[i].percent.has_value();
  }
  ComponentValues result;
  for (const Cv& v : out) result.push_back(v);
  if (!out.empty()) result.push_back(Comma());
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) result.push_back(Comma());
    result.push_back(items[i].color);
    if (write[i] && items[i].percent) result.push_back(*items[i].percent);
  }
  return Fn("color-mix", result);
}

std::optional<Cv> NormalizeInner(const Cv& v) { return NormalizeColor(v); }

}  // namespace

std::optional<ComponentValue> NormalizeColor(const ComponentValue& value) {
  if (value.kind == Cv::Kind::Token) {
    if (value.token.type == T::Hash) return ParseHex(value.token.value);
    if (value.token.type != T::Ident) return std::nullopt;
    const std::string word = Lower(value.token.value);
    if (KeywordColors().count(word)) return Ident(word);
    for (const Named& named : kNamedColors) {
      if (word == named.name) return Ident(word);
    }
    return std::nullopt;
  }
  if (value.kind != Cv::Kind::Function) return std::nullopt;
  const std::string name = Lower(value.name);
  if (name == "rgb" || name == "rgba") return ParseRgbLike(name, value.children, true, false);
  if (name == "hsl" || name == "hsla") return ParseRgbLike(name, value.children, false, true);
  if (name == "hwb") return ParseRgbLike(name, value.children, false, false);
  if (name == "lab" || name == "lch" || name == "oklab" || name == "oklch") return ParseLabLike(name, value.children);
  if (name == "color") return ParseColorFunction(value.children);
  if (name == "color-mix") return NormalizeColorMix(value.children);
  if (name == "light-dark") {
    std::vector<ComponentValues> parts = SplitOnCommas(value.children);
    if (parts.size() != 2) return std::nullopt;
    ComponentValues out;
    for (size_t i = 0; i < 2; ++i) {
      const ComponentValues trimmed = Trimmed(parts[i]);
      if (trimmed.size() != 1) return std::nullopt;
      std::optional<Cv> color = NormalizeColor(trimmed[0]);
      if (!color) return std::nullopt;
      if (i) out.push_back(Comma());
      out.push_back(*color);
    }
    return Fn("light-dark", out);
  }
  if (name == "contrast-color") {
    const ComponentValues trimmed = Trimmed(value.children);
    if (trimmed.size() != 1) return std::nullopt;
    std::optional<Cv> color = NormalizeColor(trimmed[0]);
    if (!color) return std::nullopt;
    return Fn("contrast-color", {*color});
  }
  return std::nullopt;
}

}  // namespace solar::css
