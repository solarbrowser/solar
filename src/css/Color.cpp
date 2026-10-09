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
  w = std::max(0.0, w) / 100;
  b = std::max(0.0, b) / 100;
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

namespace solar::css {

namespace {

struct SystemColor {
  const char* name;
  unsigned light;
  unsigned dark;
};

const SystemColor kSystemColors[] = {
    {"accentcolor", 0x0075ff, 0x99c8ff}, {"accentcolortext", 0xffffff, 0x000000}, {"activetext", 0xff0000, 0xff9e9e}, {"buttonborder", 0x767676, 0x6b6b6b},
    {"buttonface", 0xefefef, 0x6b6b6b}, {"buttontext", 0x000000, 0xffffff}, {"canvas", 0xffffff, 0x121212}, {"canvastext", 0x000000, 0xffffff},
    {"field", 0xffffff, 0x3b3b3b}, {"fieldtext", 0x000000, 0xffffff}, {"graytext", 0x6d6d6d, 0xa9a9a9}, {"highlight", 0xb5d5ff, 0x3b5a8a},
    {"highlighttext", 0x000000, 0xffffff}, {"linktext", 0x0000ee, 0x9e9eff}, {"mark", 0xffff00, 0x666600}, {"marktext", 0x000000, 0xffffff},
    {"selecteditem", 0xb5d5ff, 0x3b5a8a}, {"selecteditemtext", 0x000000, 0xffffff}, {"visitedtext", 0x551a8b, 0xd0adf0},
    {"activeborder", 0xffffff, 0x3b3b3b}, {"activecaption", 0xccccff, 0x3b3b3b}, {"appworkspace", 0xffffff, 0x3b3b3b}, {"background", 0xffffff, 0x121212},
    {"buttonhighlight", 0xefefef, 0x6b6b6b}, {"buttonshadow", 0xefefef, 0x6b6b6b}, {"captiontext", 0x000000, 0xffffff}, {"inactiveborder", 0xffffff, 0x3b3b3b},
    {"inactivecaption", 0xffffff, 0x3b3b3b}, {"inactivecaptiontext", 0x7f7f7f, 0xa9a9a9}, {"infobackground", 0xffffff, 0x3b3b3b}, {"infotext", 0x000000, 0xffffff},
    {"menu", 0xefefef, 0x3b3b3b}, {"menutext", 0x000000, 0xffffff}, {"scrollbar", 0xffffff, 0x3b3b3b}, {"threeddarkshadow", 0xefefef, 0x6b6b6b},
    {"threedface", 0xefefef, 0x6b6b6b}, {"threedhighlight", 0xefefef, 0x6b6b6b}, {"threedlightshadow", 0xefefef, 0x6b6b6b}, {"threedshadow", 0xefefef, 0x6b6b6b},
    {"window", 0xffffff, 0x121212}, {"windowframe", 0xffffff, 0x121212}, {"windowtext", 0x000000, 0xffffff}};

Cv RgbFromHex(unsigned rgb) {
  const double channels[3] = {((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0};
  return LegacyRgb(channels, 1);
}

}  // namespace


// ---- Color spaces ----

namespace {

enum class Space { Srgb, SrgbLinear, DisplayP3, DisplayP3Linear, A98, ProPhoto, Rec2020, XyzD50, XyzD65, Hsl, Hwb, Lab, Lch, Oklab, Oklch };

struct Col {
  Space space = Space::Srgb;
  double c[3] = {0, 0, 0};
  bool none[3] = {false, false, false};
  bool powerless[3] = {false, false, false};  // a hue that has nothing to turn: missing when mixing, 0 when read
  double alpha = 1;
  bool alphaNone = false;
};

bool IsPolar(Space s) { return s == Space::Hsl || s == Space::Hwb || s == Space::Lch || s == Space::Oklch; }
int HueIndex(Space s) { return (s == Space::Hsl || s == Space::Hwb) ? 0 : (s == Space::Lch || s == Space::Oklch) ? 2 : -1; }

const char* SpaceName(Space s) {
  switch (s) {
    case Space::Srgb: return "srgb";
    case Space::SrgbLinear: return "srgb-linear";
    case Space::DisplayP3: return "display-p3";
    case Space::DisplayP3Linear: return "display-p3-linear";
    case Space::A98: return "a98-rgb";
    case Space::ProPhoto: return "prophoto-rgb";
    case Space::Rec2020: return "rec2020";
    case Space::XyzD50: return "xyz-d50";
    case Space::XyzD65: return "xyz-d65";
    case Space::Hsl: return "hsl";
    case Space::Hwb: return "hwb";
    case Space::Lab: return "lab";
    case Space::Lch: return "lch";
    case Space::Oklab: return "oklab";
    case Space::Oklch: return "oklch";
  }
  return "";
}

std::optional<Space> SpaceByName(const std::string& name) {
  static const std::pair<const char*, Space> table[] = {
      {"srgb", Space::Srgb}, {"srgb-linear", Space::SrgbLinear}, {"display-p3", Space::DisplayP3}, {"display-p3-linear", Space::DisplayP3Linear},
      {"a98-rgb", Space::A98}, {"prophoto-rgb", Space::ProPhoto}, {"rec2020", Space::Rec2020}, {"xyz", Space::XyzD65}, {"xyz-d50", Space::XyzD50},
      {"xyz-d65", Space::XyzD65}, {"hsl", Space::Hsl}, {"hwb", Space::Hwb}, {"lab", Space::Lab}, {"lch", Space::Lch}, {"oklab", Space::Oklab}, {"oklch", Space::Oklch}};
  for (const auto& [n, s] : table) {
    if (name == n) return s;
  }
  return std::nullopt;
}

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<std::array<double, 3>, 3>;

Vec3 Mul(const Mat3& m, const Vec3& v) {
  return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2], m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

const Mat3 kSrgbToXyz = {{{0.41239079926595934, 0.357584339383878, 0.1804807884018343}, {0.21263900587151027, 0.715168678767756, 0.07219231536073371}, {0.01933081871559182, 0.11919477979462598, 0.9505321522496607}}};
const Mat3 kXyzToSrgb = {{{3.2409699419045226, -1.537383177570094, -0.4986107602930034}, {-0.9692436362808796, 1.8759675015077202, 0.04155505740717559}, {0.05563007969699366, -0.20397695888897652, 1.0569715142428786}}};
const Mat3 kP3ToXyz = {{{0.4865709486482162, 0.26566769316909306, 0.1982172852343625}, {0.2289745640697488, 0.6917385218365064, 0.079286914093745}, {0.0, 0.04511338185890264, 1.043944368900976}}};
const Mat3 kXyzToP3 = {{{2.493496911941425, -0.9313836179191239, -0.40271078445071684}, {-0.8294889695615747, 1.7626640603183463, 0.023624685841943577}, {0.03584583024378447, -0.07617238926804182, 0.9568845240076872}}};
const Mat3 kA98ToXyz = {{{0.5766690429101305, 0.1855582379065463, 0.1882286462349947}, {0.29734497525053605, 0.6273635662554661, 0.0752914584939978}, {0.02703136138641234, 0.07068885253582723, 0.9913375368376388}}};
const Mat3 kXyzToA98 = {{{2.0415879038107465, -0.5650069742788596, -0.34473135077832956}, {-0.9692436362808795, 1.8759675015077202, 0.04155505740717557}, {0.013444280632031142, -0.11836239223101838, 1.0151749943912054}}};
const Mat3 kRec2020ToXyz = {{{0.6369580483012914, 0.14461690358620832, 0.1688809751641721}, {0.2627002120112671, 0.6779980715188708, 0.05930171646986196}, {0.0, 0.028072693049087428, 1.060985057710791}}};
const Mat3 kXyzToRec2020 = {{{1.7166511879712674, -0.35567078377639233, -0.25336628137365974}, {-0.6666843518324892, 1.6164812366349395, 0.01576854581391113}, {0.017639857445310783, -0.042770613257808524, 0.9421031212354738}}};
const Mat3 kProPhotoToXyzD50 = {{{0.7977604896723027, 0.13518583717574031, 0.0313493495815248}, {0.2880711282292934, 0.7118432178101014, 0.00008565396060525902}, {0.0, 0.0, 0.8251046025104601}}};
const Mat3 kXyzD50ToProPhoto = {{{1.3457989731028281, -0.25558010007997534, -0.05110628506753401}, {-0.5446224939028347, 1.5082327413132781, 0.02053603239147973}, {0.0, 0.0, 1.2119675456389454}}};
const Mat3 kD65ToD50 = {{{1.0479297925449969, 0.022946870601609652, -0.05019226628920524}, {0.02962780877005599, 0.9904344267538799, -0.017073799063418826}, {-0.009243040646204504, 0.015055191490298152, 0.7518742814281371}}};
const Mat3 kD50ToD65 = {{{0.955473421488075, -0.02309845494876471, 0.06325924320057072}, {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323}, {0.012314014864481998, -0.020507649298898964, 1.330365926242124}}};
const Mat3 kXyzToLms = {{{0.8190224379967030, 0.3619062600528904, -0.1288737815209879}, {0.0329836539323885, 0.9292868615863434, 0.0361446663506424}, {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}}};
const Mat3 kLmsToXyz = {{{1.2268798758459243, -0.5578149944602171, 0.2813910456659647}, {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164}, {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}}};
const Mat3 kLmsToOklab = {{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193}, {1.9779985324311684, -2.4285922420485799, 0.4505937096174110}, {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}}};
const Mat3 kOklabToLms = {{{1.0, 0.3963377773761749, 0.2158037573099136}, {1.0, -0.1055613458156586, -0.0638541728258133}, {1.0, -0.0894841775298119, -1.2914855480194092}}};

double SrgbDecode(double v) {
  const double a = std::fabs(v);
  const double r = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
  return v < 0 ? -r : r;
}
double SrgbEncode(double v) {
  const double a = std::fabs(v);
  const double r = a > 0.0031308 ? 1.055 * std::pow(a, 1 / 2.4) - 0.055 : 12.92 * a;
  return v < 0 ? -r : r;
}
double A98Decode(double v) { return std::copysign(std::pow(std::fabs(v), 563.0 / 256), v); }
double A98Encode(double v) { return std::copysign(std::pow(std::fabs(v), 256.0 / 563), v); }
double ProPhotoDecode(double v) {
  const double a = std::fabs(v);
  return std::copysign(a <= 16.0 / 512 ? a / 16 : std::pow(a, 1.8), v);
}
double ProPhotoEncode(double v) {
  const double a = std::fabs(v);
  return std::copysign(a >= 1.0 / 512 ? std::pow(a, 1 / 1.8) : 16 * a, v);
}
constexpr double kRecAlpha = 1.09929682680944, kRecBeta = 0.018053968510807;
double Rec2020Decode(double v) {
  const double a = std::fabs(v);
  return std::copysign(a < kRecBeta * 4.5 ? a / 4.5 : std::pow((a + kRecAlpha - 1) / kRecAlpha, 1 / 0.45), v);
}
double Rec2020Encode(double v) {
  const double a = std::fabs(v);
  return std::copysign(a >= kRecBeta ? kRecAlpha * std::pow(a, 0.45) - (kRecAlpha - 1) : 4.5 * a, v);
}

const Vec3 kD50White = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
constexpr double kEpsilon = 216.0 / 24389, kKappa = 24389.0 / 27;

Vec3 XyzD50ToLab(const Vec3& xyz) {
  Vec3 f;
  for (int i = 0; i < 3; ++i) {
    const double t = xyz[i] / kD50White[i];
    f[i] = t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16) / 116;
  }
  return {116 * f[1] - 16, 500 * (f[0] - f[1]), 200 * (f[1] - f[2])};
}
Vec3 LabToXyzD50(const Vec3& lab) {
  const double f1 = (lab[0] + 16) / 116, f0 = lab[1] / 500 + f1, f2 = f1 - lab[2] / 200;
  const auto inverse = [](double f, double l) { return std::pow(f, 3) > kEpsilon ? std::pow(f, 3) : (116 * f - 16) / kKappa; (void)l; };
  const double x = inverse(f0, 0) * kD50White[0];
  const double y = (lab[0] > kKappa * kEpsilon ? std::pow((lab[0] + 16) / 116, 3) : lab[0] / kKappa) * kD50White[1];
  const double z = inverse(f2, 0) * kD50White[2];
  return {x, y, z};
}

Vec3 XyzToOklab(const Vec3& xyz) {
  Vec3 lms = Mul(kXyzToLms, xyz);
  for (double& v : lms) v = std::cbrt(v);
  return Mul(kLmsToOklab, lms);
}
Vec3 OklabToXyz(const Vec3& lab) {
  Vec3 lms = Mul(kOklabToLms, lab);
  for (double& v : lms) v = v * v * v;
  return Mul(kLmsToXyz, lms);
}

// To and from the hue-based forms.
Vec3 RgbToHsl(const Vec3& rgb) {
  const double max = std::max({rgb[0], rgb[1], rgb[2]}), min = std::min({rgb[0], rgb[1], rgb[2]});
  double h = NAN, s = 0;
  const double l = (min + max) / 2, d = max - min;
  if (d != 0) {
    s = (l == 0 || l == 1) ? 0 : (max - l) / std::min(l, 1 - l);
    if (max == rgb[0]) h = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6 : 0);
    else if (max == rgb[1]) h = (rgb[2] - rgb[0]) / d + 2;
    else h = (rgb[0] - rgb[1]) / d + 4;
    h *= 60;
  }
  return {h, s * 100, l * 100};
}
Vec3 HslToRgb(const Vec3& hsl) {
  double h = std::fmod(hsl[0], 360);
  if (h < 0) h += 360;
  const double s = hsl[1] / 100, l = hsl[2] / 100;
  const auto f = [&](double n) {
    const double k = std::fmod(n + h / 30, 12);
    const double a = s * std::min(l, 1 - l);
    return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
  };
  return {f(0), f(8), f(4)};
}
Vec3 RgbToHwb(const Vec3& rgb) {
  const Vec3 hsl = RgbToHsl(rgb);
  const double w = std::min({rgb[0], rgb[1], rgb[2]}), b = 1 - std::max({rgb[0], rgb[1], rgb[2]});
  return {hsl[0], w * 100, b * 100};
}
Vec3 HwbToRgb(const Vec3& hwb) {
  double w = hwb[1] / 100, b = hwb[2] / 100;  // not clamped: more than the whole is shared out between them
  if (w + b >= 1) {
    const double gray = w / (w + b);
    return {gray, gray, gray};
  }
  Vec3 rgb = HslToRgb({hwb[0], 100, 50});
  for (double& v : rgb) v = v * (1 - w - b) + w;
  return rgb;
}

Vec3 LabToLch(const Vec3& lab) {
  const double c = std::hypot(lab[1], lab[2]);
  double h = std::atan2(lab[2], lab[1]) * 180 / 3.14159265358979323846;
  if (h < 0) h += 360;
  return {lab[0], c, h};
}
Vec3 LchToLab(const Vec3& lch) {
  const double h = lch[2] * 3.14159265358979323846 / 180;
  return {lch[0], lch[1] * std::cos(h), lch[1] * std::sin(h)};
}

// A color of any space as XYZ with the D65 white; the components that are none are 0.
Vec3 ToXyzD65(const Col& col) {
  const Vec3 v = {col.none[0] ? 0 : col.c[0], col.none[1] ? 0 : col.c[1], col.none[2] ? 0 : col.c[2]};
  const auto each = [&](double (*f)(double)) { return Vec3{f(v[0]), f(v[1]), f(v[2])}; };
  switch (col.space) {
    case Space::Srgb: return Mul(kSrgbToXyz, each(SrgbDecode));
    case Space::SrgbLinear: return Mul(kSrgbToXyz, v);
    case Space::DisplayP3: return Mul(kP3ToXyz, each(SrgbDecode));
    case Space::DisplayP3Linear: return Mul(kP3ToXyz, v);
    case Space::A98: return Mul(kA98ToXyz, each(A98Decode));
    case Space::ProPhoto: return Mul(kD50ToD65, Mul(kProPhotoToXyzD50, each(ProPhotoDecode)));
    case Space::Rec2020: return Mul(kRec2020ToXyz, each(Rec2020Decode));
    case Space::XyzD50: return Mul(kD50ToD65, v);
    case Space::XyzD65: return v;
    case Space::Hsl: return Mul(kSrgbToXyz, Vec3{SrgbDecode(HslToRgb(v)[0]), SrgbDecode(HslToRgb(v)[1]), SrgbDecode(HslToRgb(v)[2])});
    case Space::Hwb: return Mul(kSrgbToXyz, Vec3{SrgbDecode(HwbToRgb(v)[0]), SrgbDecode(HwbToRgb(v)[1]), SrgbDecode(HwbToRgb(v)[2])});
    case Space::Lab: return Mul(kD50ToD65, LabToXyzD50(v));
    case Space::Lch: return Mul(kD50ToD65, LabToXyzD50(LchToLab(v)));
    case Space::Oklab: return OklabToXyz(v);
    case Space::Oklch: return OklabToXyz(LchToLab(v));
  }
  return v;
}

Vec3 FromXyzD65(const Vec3& xyz, Space space) {
  const auto each = [&](const Vec3& v, double (*f)(double)) { return Vec3{f(v[0]), f(v[1]), f(v[2])}; };
  switch (space) {
    case Space::Srgb: return each(Mul(kXyzToSrgb, xyz), SrgbEncode);
    case Space::SrgbLinear: return Mul(kXyzToSrgb, xyz);
    case Space::DisplayP3: return each(Mul(kXyzToP3, xyz), SrgbEncode);
    case Space::DisplayP3Linear: return Mul(kXyzToP3, xyz);
    case Space::A98: return each(Mul(kXyzToA98, xyz), A98Encode);
    case Space::ProPhoto: return each(Mul(kXyzD50ToProPhoto, Mul(kD65ToD50, xyz)), ProPhotoEncode);
    case Space::Rec2020: return each(Mul(kXyzToRec2020, xyz), Rec2020Encode);
    case Space::XyzD50: return Mul(kD65ToD50, xyz);
    case Space::XyzD65: return xyz;
    case Space::Hsl: return RgbToHsl(each(Mul(kXyzToSrgb, xyz), SrgbEncode));
    case Space::Hwb: return RgbToHwb(each(Mul(kXyzToSrgb, xyz), SrgbEncode));
    case Space::Lab: return XyzD50ToLab(Mul(kD65ToD50, xyz));
    case Space::Lch: return LabToLch(XyzD50ToLab(Mul(kD65ToD50, xyz)));
    case Space::Oklab: return XyzToOklab(xyz);
    case Space::Oklch: return LabToLch(XyzToOklab(xyz));
  }
  return xyz;
}

// Which component of another space takes a missing component over: the analogous ones.
int Analogous(Space from, int index, Space to) {
  const auto group = [](Space s, int i) -> int {
    switch (s) {
      case Space::Srgb: case Space::SrgbLinear: case Space::DisplayP3: case Space::DisplayP3Linear: case Space::A98: case Space::ProPhoto: case Space::Rec2020:
      case Space::XyzD50: case Space::XyzD65:
        return i;  // red/x, green/y, blue/z
      case Space::Hsl: return i == 0 ? 10 : i == 1 ? 11 : 12;       // hue, saturation, lightness
      case Space::Hwb: return i == 0 ? 10 : i == 1 ? 13 : 14;       // hue, whiteness, blackness
      case Space::Lab: case Space::Oklab: return i == 0 ? 12 : i == 1 ? 15 : 16;   // lightness, a, b
      case Space::Lch: case Space::Oklch: return i == 0 ? 12 : i == 1 ? 11 : 10;   // lightness, chroma, hue
    }
    return -1;
  };
  const int g = group(from, index);
  for (int i = 0; i < 3; ++i) {
    if (group(to, i) == g) return i;
  }
  return -1;
}

bool IsRgbLike(Space s) { return s == Space::Srgb || s == Space::SrgbLinear || s == Space::DisplayP3 || s == Space::DisplayP3Linear || s == Space::A98 || s == Space::ProPhoto || s == Space::Rec2020 || s == Space::XyzD50 || s == Space::XyzD65; }

Col Convert(const Col& in, Space to) {
  if (in.space == to) return in;
  Col out;
  out.space = to;
  out.alpha = in.alpha;
  out.alphaNone = in.alphaNone;
  const Vec3 v = FromXyzD65(ToXyzD65(in), to);
  for (int i = 0; i < 3; ++i) out.c[i] = v[i];
  // Missing components carry over to the analogous ones.
  for (int i = 0; i < 3; ++i) {
    if (!in.none[i]) continue;
    const int target = Analogous(in.space, i, to);
    if (target >= 0) out.none[target] = true;
  }
  // A hue that has nothing to turn is missing.
  const int hue = HueIndex(to);
  if (hue >= 0 && !out.none[hue]) {
    bool powerless = false;
    if (to == Space::Hsl) powerless = out.c[1] <= 0.0001 || out.c[2] <= 0 || out.c[2] >= 100 - 1e-9;
    else if (to == Space::Hwb) powerless = out.c[1] + out.c[2] >= 100 - 1e-9;
    else if (to == Space::Lch) powerless = out.c[1] <= 0.0015;
    else if (to == Space::Oklch) powerless = out.c[1] <= 0.000004;
    if (powerless && !std::isnan(out.c[hue]) && !IsPolar(in.space)) out.powerless[hue] = true;
    if (std::isnan(out.c[hue])) out.powerless[hue] = true;
  }
  for (int i = 0; i < 3; ++i) {
    if (std::isnan(out.c[i])) out.c[i] = 0;
  }
  return out;
}

// ---- Reading a normalized color into a Col ----

struct ChannelRef {
  double percentScale;
  bool angle;
};

ChannelRef ReferenceOf(const std::string& fn, Space space, int index) {
  if (fn == "color") return {1, false};
  if (fn == "rgb") return {255, false};
  if (fn == "hsl" || fn == "hwb") return index == 0 ? ChannelRef{360, true} : ChannelRef{100, false};
  if (fn == "lab") return index == 0 ? ChannelRef{100, false} : ChannelRef{125, false};
  if (fn == "lch") return index == 0 ? ChannelRef{100, false} : index == 1 ? ChannelRef{150, false} : ChannelRef{360, true};
  if (fn == "oklab") return index == 0 ? ChannelRef{1, false} : ChannelRef{0.4, false};
  if (fn == "oklch") return index == 0 ? ChannelRef{1, false} : index == 1 ? ChannelRef{0.4, false} : ChannelRef{360, true};
  (void)space;
  return {1, false};
}

// A channel's value: a number, a percentage of the reference, an angle, a calculation; none.
bool ChannelValue(const Cv& v, const ChannelRef& ref, double& value, bool& isNone) {
  isNone = false;
  if (v.IsIdent() && Lower(v.token.value) == "none") {
    isNone = true;
    value = 0;
    return true;
  }
  const std::optional<MathValue> m = EvaluateNumeric(v);
  if (!m) return false;
  switch (m->kind) {
    case MathKind::Number: value = m->value; return true;
    case MathKind::Percentage: value = m->value * ref.percentScale / 100; return true;
    case MathKind::Angle:
      if (!ref.angle) return false;
      value = m->value;
      return true;
    default: return false;
  }
}

std::optional<Col> ReadColor(const Cv& v, const std::string& currentColor, const std::string& scheme);

std::optional<Col> ReadFunction(const Cv& v, const std::string& currentColor, const std::string& scheme);

// Whatever component values are written in a color function, as items before and after the slash.
bool SplitItems(const ComponentValues& children, ComponentValues& channels, ComponentValues& alpha, bool& slash) {
  slash = false;
  for (const Cv& c : children) {
    if (c.IsWhitespace()) continue;
    if (c.IsToken(T::Comma)) continue;
    if (c.IsDelim('/')) {
      slash = true;
      continue;
    }
    (slash ? alpha : channels).push_back(c);
  }
  return true;
}

std::optional<Col> ReadColor(const Cv& v, const std::string& currentColor, const std::string& scheme) {
  if (v.kind == Cv::Kind::Token && v.token.type == T::Ident) {
    std::optional<Cv> computed = ComputeColor(v, currentColor, scheme);
    if (!computed) return std::nullopt;
    return ReadColor(*computed, currentColor, scheme);
  }
  if (v.kind != Cv::Kind::Function) return std::nullopt;
  return ReadFunction(v, currentColor, scheme);
}

std::optional<Col> ReadFunction(const Cv& v, const std::string& currentColor, const std::string& scheme) {
  const std::string name = Lower(v.name);
  ComponentValues channels, alpha;
  bool slash = false;
  SplitItems(v.children, channels, alpha, slash);
  if (name == "color-mix" || name == "light-dark" || name == "contrast-color") {
    std::optional<Cv> computed = ComputeColor(v, currentColor, scheme);
    if (!computed || (computed->kind == Cv::Kind::Function && Lower(computed->name) == name)) return std::nullopt;
    return ReadColor(*computed, currentColor, scheme);
  }
  if (!channels.empty() && channels[0].IsIdent() && Lower(channels[0].token.value) == "from") {
    std::optional<Cv> computed = ComputeColor(v, currentColor, scheme);
    if (!computed || (computed->kind == Cv::Kind::Function && !channels.empty() && computed->children.size() > 0 && computed->children[0].IsIdent() && Lower(computed->children[0].token.value) == "from")) return std::nullopt;
    return ReadColor(*computed, currentColor, scheme);
  }
  Col col;
  std::string fn = name == "rgba" ? "rgb" : name == "hsla" ? "hsl" : name;
  if (!slash && channels.size() == 4 && fn != "color") {
    // The legacy syntax: the alpha is the fourth value.
    alpha.push_back(channels[3]);
    channels.pop_back();
    slash = true;
  }
  size_t first = 0;
  if (fn == "color") {
    if (channels.empty() || !channels[0].IsIdent()) return std::nullopt;
    std::optional<Space> space = SpaceByName(Lower(channels[0].token.value));
    if (!space) return std::nullopt;
    col.space = *space;
    first = 1;
  } else if (fn == "rgb") {
    col.space = Space::Srgb;
  } else if (std::optional<Space> space = SpaceByName(fn)) {
    col.space = *space;
  } else {
    return std::nullopt;
  }
  if (channels.size() != first + 3) return std::nullopt;
  for (int i = 0; i < 3; ++i) {
    const ChannelRef ref = ReferenceOf(fn, col.space, i);
    double value;
    bool isNone;
    if (!ChannelValue(channels[first + i], ref, value, isNone)) return std::nullopt;
    col.c[i] = fn == "rgb" ? value / 255 : value;
    col.none[i] = isNone;
  }
  if (fn == "hsl" || fn == "hwb") {
    // s, l, w, b are percentages: 100 is full.
  }
  if (slash) {
    if (alpha.size() != 1) return std::nullopt;
    double value;
    bool isNone;
    if (!ChannelValue(alpha[0], ChannelRef{1, false}, value, isNone)) return std::nullopt;
    col.alpha = std::max(0.0, std::min(1.0, std::isnan(value) ? 0.0 : value));
    col.alphaNone = isNone;
  }
  return col;
}

// ---- Writing a Col ----

Cv N(double v) {
  if (!std::isfinite(v)) return Fn("calc", {Ident(std::isnan(v) ? "NaN" : v < 0 ? "-infinity" : "infinity")});
  Cv c = Num(0);
  c.token.number = Round6(v);
  c.token.isInteger = c.token.number == std::floor(c.token.number);
  return c;
}

Cv NN(double v) {  // eight decimals, for color()'s channels
  Cv c = Num(0);
  c.token.number = Round8(v);
  return c;
}

void AppendAlphaCol(const Col& col, ComponentValues& out) {
  if (col.alphaNone) {
    out.push_back(Slash());
    out.push_back(Ident("none"));
  } else if (col.alpha != 1) {
    out.push_back(Slash());
    out.push_back(N(std::max(0.0, std::min(1.0, col.alpha))));
  }
}

Cv Write(const Col& in) {
  Col col = in;
  for (int i = 0; i < 3; ++i) {
    if (!col.none[i] && std::isnan(col.c[i])) col.c[i] = 0;
  }
  ComponentValues out;
  const auto chan = [&](int i, double value, bool percent = false) {
    if (col.none[i]) return Ident("none");
    return percent ? Pct(value) : N(value);
  };
  switch (col.space) {
    case Space::Hsl:
    case Space::Hwb: {
      bool anyNone = col.alphaNone || col.none[0] || col.none[1] || col.none[2];
      if (!anyNone) return Write(Convert(col, Space::Srgb));
      out.push_back(chan(0, std::fmod(std::fmod(col.c[0], 360) + 360, 360)));
      out.push_back(chan(1, col.c[1], true));
      out.push_back(chan(2, col.c[2], true));
      AppendAlphaCol(col, out);
      return Fn(SpaceName(col.space), out);
    }
    case Space::Lab:
    case Space::Oklab: {
      const bool ok = col.space == Space::Oklab;
      out.push_back(chan(0, std::max(0.0, std::min(ok ? 1.0 : 100.0, col.c[0]))));
      out.push_back(chan(1, col.c[1]));
      out.push_back(chan(2, col.c[2]));
      AppendAlphaCol(col, out);
      return Fn(SpaceName(col.space), out);
    }
    case Space::Lch:
    case Space::Oklch: {
      const bool ok = col.space == Space::Oklch;
      out.push_back(chan(0, std::max(0.0, std::min(ok ? 1.0 : 100.0, col.c[0]))));
      out.push_back(chan(1, std::max(0.0, col.c[1])));
      out.push_back(chan(2, std::fmod(std::fmod(col.c[2], 360) + 360, 360)));
      AppendAlphaCol(col, out);
      return Fn(SpaceName(col.space), out);
    }
    default: {
      out.push_back(Ident(SpaceName(col.space)));
      for (int i = 0; i < 3; ++i) out.push_back(col.none[i] ? Ident("none") : NN(col.c[i]));
      AppendAlphaCol(col, out);
      return Fn("color", out);
    }
  }
}

// ---- Relative colors ----

std::optional<Cv> ComputeRelative(const Cv& v, const std::string& currentColor, const std::string& scheme) {
  const std::string name = Lower(v.name);
  std::string fn = name == "rgba" ? "rgb" : name == "hsla" ? "hsl" : name;
  ComponentValues channels, alpha;
  bool slash = false;
  SplitItems(v.children, channels, alpha, slash);
  // from <color> [<space>] c1 c2 c3
  if (channels.size() < 2) return std::nullopt;
  std::optional<Col> origin = ReadColor(channels[1], currentColor, scheme);
  if (!origin) return std::nullopt;
  size_t at = 2;
  Space target = Space::Srgb;
  if (fn == "color") {
    if (channels.size() <= at || !channels[at].IsIdent()) return std::nullopt;
    std::optional<Space> space = SpaceByName(Lower(channels[at].token.value));
    if (!space) return std::nullopt;
    target = *space;
    ++at;
  } else if (fn == "rgb") {
    target = Space::Srgb;
  } else if (std::optional<Space> space = SpaceByName(fn)) {
    target = *space;
  } else {
    return std::nullopt;
  }
  if (channels.size() != at + 3) return std::nullopt;
  const Col source = Convert(*origin, target);
  // The channel keywords and their values; rgb()'s are 0-255.
  std::vector<std::string> names;
  if (fn == "rgb") names = {"r", "g", "b"};
  else if (fn == "hsl") names = {"h", "s", "l"};
  else if (fn == "hwb") names = {"h", "w", "b"};
  else if (fn == "lab" || fn == "oklab") names = {"l", "a", "b"};
  else if (fn == "lch" || fn == "oklch") names = {"l", "c", "h"};
  else if (target == Space::XyzD50 || target == Space::XyzD65) names = {"x", "y", "z"};
  else names = {"r", "g", "b"};
  double values[4];
  for (int i = 0; i < 3; ++i) values[i] = (fn == "rgb" ? source.c[i] * 255 : source.c[i]);
  values[3] = source.alpha;
  const auto substitute = [&](const Cv& expression, auto&& self) -> Cv {
    if (expression.kind == Cv::Kind::Token) {
      if (expression.IsIdent()) {
        const std::string word = Lower(expression.token.value);
        for (int i = 0; i < 3; ++i) {
          if (word == names[i]) return N(values[i]);
        }
        if (word == "alpha") return N(values[3]);
      }
      return expression;
    }
    Cv copy = expression;
    for (Cv& child : copy.children) child = self(child, self);
    return copy;
  };
  Col result;
  result.space = target;
  for (int i = 0; i < 3; ++i) {
    const Cv& item = channels[at + i];
    const ChannelRef ref = ReferenceOf(fn, target, i);
    // A keyword on its own keeps the channel as it is, missing or not.
    if (item.IsIdent()) {
      const std::string word = Lower(item.token.value);
      if (word == "none") { result.none[i] = true; continue; }
      if (word == names[i] || [&] { for (int j = 0; j < 3; ++j) if (word == names[j]) return true; return false; }()) {
        for (int j = 0; j < 3; ++j) {
          if (word == names[j]) {
            result.c[i] = source.c[j];
            result.none[i] = source.none[j];
          }
        }
        continue;
      }
    }
    double value;
    bool isNone;
    if (!ChannelValue(substitute(item, substitute), ref, value, isNone)) return std::nullopt;
    result.c[i] = fn == "rgb" ? value / 255 : value;
    result.none[i] = isNone;
  }
  result.alpha = source.alpha;
  result.alphaNone = source.alphaNone;
  if (slash) {
    if (alpha.size() != 1) return std::nullopt;
    const Cv& item = alpha[0];
    if (item.IsIdent() && Lower(item.token.value) == "alpha") {
      result.alpha = source.alpha;
      result.alphaNone = source.alphaNone;
    } else {
      double value;
      bool isNone;
      if (!ChannelValue(substitute(item, substitute), ChannelRef{1, false}, value, isNone)) return std::nullopt;
      result.alpha = std::max(0.0, std::min(1.0, std::isnan(value) ? 0.0 : value));
      result.alphaNone = isNone;
    }
  }
  return Write(result);
}

// ---- color-mix ----

struct MixItem {
  Col color;
  double percent;
};

double ArcDistance(double a, double b) { return b - a; }

void AdjustHues(double& a, double& b, const std::string& method) {
  a = std::fmod(std::fmod(a, 360) + 360, 360);
  b = std::fmod(std::fmod(b, 360) + 360, 360);
  if (method == "shorter") {
    const double d = b - a;
    if (d > 180) a += 360;
    else if (d < -180) b += 360;
  } else if (method == "longer") {
    const double d = b - a;
    if (d > 0 && d < 180) a += 360;
    else if (d > -180 && d <= 0) b += 360;
  } else if (method == "increasing") {
    if (b < a) b += 360;
  } else if (method == "decreasing") {
    if (a < b) a += 360;
  }
  (void)ArcDistance;
}

std::optional<Cv> ComputeColorMix(const Cv& v, const std::string& currentColor, const std::string& scheme) {
  std::vector<ComponentValues> parts = SplitOnCommas(v.children);
  Space space = Space::Oklab;
  std::string hueMethod = "shorter";
  const ComponentValues method = Trimmed(parts[0]);
  if (!method.empty() && method[0].IsIdent() && Lower(method[0].token.value) == "in") {
    std::vector<Cv> words;
    for (const Cv& c : method) {
      if (!c.IsWhitespace()) words.push_back(c);
    }
    std::optional<Space> s = SpaceByName(Lower(words[1].token.value));
    if (!s) return std::nullopt;
    space = *s;
    if (words.size() == 4) hueMethod = Lower(words[2].token.value);
    parts.erase(parts.begin());
  }
  std::vector<MixItem> items;
  for (const ComponentValues& part : parts) {
    std::vector<Cv> pieces;
    for (const Cv& c : Trimmed(part)) {
      if (!c.IsWhitespace()) pieces.push_back(c);
    }
    if (pieces.empty()) return std::nullopt;
    MixItem item;
    item.percent = -1;
    const Cv* colorCv = &pieces[0];
    if (pieces.size() == 2) {
      const bool firstIsPercent = (pieces[0].kind == Cv::Kind::Token && pieces[0].token.type == T::Percentage) || (pieces[0].kind == Cv::Kind::Function && IsMathFunctionName(pieces[0].name));
      const Cv& pct = firstIsPercent ? pieces[0] : pieces[1];
      colorCv = firstIsPercent ? &pieces[1] : &pieces[0];
      const std::optional<MathValue> m = EvaluateNumeric(pct);
      if (!m || m->kind != MathKind::Percentage) return std::nullopt;
      item.percent = m->value;
    }
    std::optional<Col> col = ReadColor(*colorCv, currentColor, scheme);
    if (!col) return std::nullopt;
    item.color = Convert(*col, space);
    for (int i = 0; i < 3; ++i) {
      if (item.color.powerless[i]) item.color.none[i] = true;
    }
    items.push_back(item);
  }
  if (items.empty()) return std::nullopt;
  // Percentages: the missing ones share what is left; the total is scaled to 100% (and the alpha lessened if it was less).
  double given = 0;
  size_t omitted = 0;
  for (const MixItem& item : items) {
    if (item.percent >= 0) given += item.percent; else ++omitted;
  }
  for (MixItem& item : items) {
    if (item.percent < 0) item.percent = omitted ? std::max(0.0, 100 - given) / omitted : 0;
  }
  double total = 0;
  for (const MixItem& item : items) total += item.percent;
  double alphaMultiplier = 1;
  if (total <= 0) {
    // Nothing asked for: an even mix, made fully transparent.
    for (MixItem& item : items) item.percent = 100.0 / items.size();
    total = 100;
    alphaMultiplier = 0;
  } else if (total < 100) {
    alphaMultiplier = total / 100;
  }
  for (MixItem& item : items) item.percent = item.percent / total;

  // Interpolate pairwise from the left (the spec's n-ary form folds this way).
  Col acc = items[0].color;
  double accWeight = items[0].percent;
  for (size_t k = 1; k < items.size(); ++k) {
    const Col& next = items[k].color;
    const double w2 = items[k].percent;
    const double w = accWeight + w2;
    const double t = w == 0 ? 0 : w2 / w;
    Col result;
    result.space = space;
    // Missing components take the other's value; both missing stay missing.
    double a[3], b[3];
    bool none[3];
    for (int i = 0; i < 3; ++i) {
      none[i] = acc.none[i] && next.none[i];
      a[i] = acc.none[i] ? (next.none[i] ? 0 : next.c[i]) : acc.c[i];
      b[i] = next.none[i] ? (acc.none[i] ? 0 : acc.c[i]) : next.c[i];
    }
    double alphaA = acc.alphaNone ? (next.alphaNone ? 1 : next.alpha) : acc.alpha;
    double alphaB = next.alphaNone ? (acc.alphaNone ? 1 : acc.alpha) : next.alpha;
    const int hue = HueIndex(space);
    if (hue >= 0) AdjustHues(a[hue], b[hue], hueMethod);
    // Premultiply (not the hue).
    for (int i = 0; i < 3; ++i) {
      if (i == hue) continue;
      a[i] *= alphaA;
      b[i] *= alphaB;
    }
    const double mixedAlpha = alphaA * (1 - t) + alphaB * t;
    for (int i = 0; i < 3; ++i) {
      double value = a[i] * (1 - t) + b[i] * t;
      if (i != hue && mixedAlpha != 0) value /= mixedAlpha;
      result.c[i] = value;
      result.none[i] = none[i];
    }
    result.alpha = mixedAlpha;
    result.alphaNone = acc.alphaNone && next.alphaNone;
    acc = result;
    accWeight = w;
  }
  acc.alpha = acc.alphaNone ? acc.alpha : acc.alpha * alphaMultiplier;
  if (acc.alphaNone) acc.alpha = 1;
  return Write(acc);
}

}  // namespace

std::optional<ComponentValue> ComputeColor(const ComponentValue& specified, const ComputeContext& context) {
  return ComputeColor(ResolveRelativeLengths(specified, context), context.currentColor, context.colorScheme);
}

std::optional<ComponentValue> ComputeColor(const ComponentValue& specified, const std::string& currentColor, const std::string& scheme) {
  if (specified.kind == Cv::Kind::Token && specified.token.type == T::Ident) {
    const std::string word = Lower(specified.token.value);
    if (word == "currentcolor") {
      Cv parsed;
      if (ParseComponentValue(currentColor, parsed)) return parsed;
      return std::nullopt;
    }
    if (word == "transparent") {
      const double none[3] = {0, 0, 0};
      return LegacyRgb(none, 0);
    }
    for (const Named& named : kNamedColors) {
      if (word == named.name) return RgbFromHex(named.rgb);
    }
    for (const SystemColor& system : kSystemColors) {
      if (word == system.name) return RgbFromHex(scheme == "dark" ? system.dark : system.light);
    }
    return std::nullopt;
  }
  if (specified.kind != Cv::Kind::Function) return specified;
  const std::string name = Lower(specified.name);
  if (name == "light-dark") {
    std::vector<ComponentValues> parts = SplitOnCommas(specified.children);
    if (parts.size() != 2) return std::nullopt;
    const ComponentValues chosen = Trimmed(parts[scheme == "dark" ? 1 : 0]);
    if (chosen.size() != 1) return std::nullopt;
    return ComputeColor(chosen[0], currentColor, scheme);
  }
  if (name == "color-mix") return ComputeColorMix(specified, currentColor, scheme);
  if (name == "contrast-color") {
    const ComponentValues inner = Trimmed(specified.children);
    if (inner.size() != 1) return std::nullopt;
    std::optional<Col> col = ReadColor(inner[0], currentColor, scheme);
    if (!col) return std::nullopt;
    const Vec3 xyz = ToXyzD65(Convert(*col, Space::Srgb));
    const double y = xyz[1];
    // The one of black and white that contrasts more.
    const double lightContrast = 1.05 / (y + 0.05), darkContrast = (y + 0.05) / 0.05;
    const double v[3] = {lightContrast > darkContrast ? 1.0 : 0.0, lightContrast > darkContrast ? 1.0 : 0.0, lightContrast > darkContrast ? 1.0 : 0.0};
    return LegacyRgb(v, 1);
  }
  // Relative colors.
  ComponentValues channels, alpha;
  bool slash = false;
  SplitItems(specified.children, channels, alpha, slash);
  if (!channels.empty() && channels[0].IsIdent() && Lower(channels[0].token.value) == "from") return ComputeRelative(specified, currentColor, scheme);
  // rgb()/rgba() in the legacy form are done.
  const bool legacyRgb = (name == "rgb" || name == "rgba") && !specified.children.empty() && std::any_of(specified.children.begin(), specified.children.end(), [](const Cv& c) { return c.IsToken(T::Comma); });
  if (legacyRgb) return specified;
  // The rest: calculations worked out, and the colors that could be legacy rgb() written as that.
  std::optional<Col> col = ReadFunction(specified, currentColor, scheme);
  if (!col) return specified;
  if (name == "rgb" || name == "rgba") {
    if (!col->none[0] && !col->none[1] && !col->none[2] && !col->alphaNone) {
      const double rgb[3] = {col->c[0], col->c[1], col->c[2]};
      return LegacyRgb(rgb, col->alpha);
    }
    Col asRgb = *col;
    return Write(asRgb);
  }
  if ((name == "hsl" || name == "hsla" || name == "hwb") && !col->none[0] && !col->none[1] && !col->none[2] && !col->alphaNone) {
    const Col srgb = Convert(*col, Space::Srgb);
    const double rgb[3] = {srgb.c[0], srgb.c[1], srgb.c[2]};
    return LegacyRgb(rgb, srgb.alpha);
  }
  return Write(*col);
}

}  // namespace solar::css
