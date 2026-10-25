/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// A CSS color as a computed value gives it, as an sRGB color of bytes.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "solar/paint/Paint.h"

namespace solar::paint {

namespace {

// The numbers of a color function's arguments: separated by spaces, commas or a slash; `none` is 0.
struct Arguments {
  std::vector<double> values;
  std::vector<bool> percent;
  bool hasAlpha = false;
  double alpha = 1;
};

bool ReadArguments(const std::string& inside, Arguments& out) {
  std::string token;
  bool afterSlash = false;
  const auto flush = [&]() -> bool {
    if (token.empty()) return true;
    std::string t = token;
    token.clear();
    bool isPercent = false;
    if (t == "none") t = "0";
    if (t.back() == '%') {
      isPercent = true;
      t.pop_back();
    } else if (t.size() > 3 && t.compare(t.size() - 3, 3, "deg") == 0) {
      t.resize(t.size() - 3);
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    if (afterSlash) {
      out.hasAlpha = true;
      out.alpha = isPercent ? v / 100 : v;
    } else {
      out.values.push_back(v);
      out.percent.push_back(isPercent);
    }
    return true;
  };
  for (char c : inside) {
    if (c == ' ' || c == ',' || c == '\t' || c == '\n') {
      if (!flush()) return false;
    } else if (c == '/') {
      if (!flush()) return false;
      afterSlash = true;
    } else {
      token += c;
    }
  }
  return flush();
}

double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

uint8_t Byte(double v) { return static_cast<uint8_t>(std::lround(Clamp01(v) * 255)); }

// Linear light to the sRGB transfer curve.
double Encode(double linear) {
  const double a = std::fabs(linear);
  const double e = a <= 0.0031308 ? 12.92 * a : 1.055 * std::pow(a, 1 / 2.4) - 0.055;
  return linear < 0 ? -e : e;
}

Color FromLinear(double r, double g, double b, double alpha) {
  Color c;
  c.r = Byte(Encode(r));
  c.g = Byte(Encode(g));
  c.b = Byte(Encode(b));
  c.a = Byte(alpha);
  return c;
}

Color FromOklab(double L, double a, double b, double alpha) {
  const double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
  const double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
  const double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
  const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  return FromLinear(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s, alpha);
}

// CIE Lab (D50) to XYZ (D50), then to linear sRGB with the Bradford adaptation to D65.
Color FromLab(double L, double a, double b, double alpha) {
  const double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
  const double eps = 216.0 / 24389, kappa = 24389.0 / 27;
  const auto finv = [&](double t) { return t * t * t > eps ? t * t * t : (116 * t - 16) / kappa; };
  const double X = finv(fx) * 0.3457 / 0.3585;
  const double Y = L > kappa * eps ? std::pow((L + 16) / 116, 3) : L / kappa;
  const double Z = finv(fz) * (1 - 0.3457 - 0.3585) / 0.3585;
  const double x = 0.9554734527042182 * X - 0.023098536874261423 * Y + 0.0632593086610217 * Z;
  const double y = -0.028369706963208136 * X + 1.0099954580058226 * Y + 0.021041398966943008 * Z;
  const double z = 0.012314001688319899 * X - 0.020507696433477912 * Y + 1.3303659366080753 * Z;
  return FromLinear(3.2409699419045226 * x - 1.537383177570094 * y - 0.4986107602930034 * z, -0.9692436362808796 * x + 1.8759675015077202 * y + 0.04155505740717559 * z,
                    0.05563007969699366 * x - 0.20397695888897652 * y + 1.0569715142428786 * z, alpha);
}

}  // namespace

Color ParseColor(const std::string& given) {
  std::string text;
  for (char c : given) text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t open = text.find('(');
  const size_t close = text.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return Color{};  // transparent, or a keyword a computed value does not have
  const std::string name = text.substr(0, open);
  if (name == "color") {
    // color(srgb r g b / a): the first word is the color space, and the rest are numbers. Spaces other than sRGB are taken as it.
    const size_t start = text.find_first_not_of(" \t", open + 1);
    const size_t space = start == std::string::npos ? std::string::npos : text.find_first_of(" \t", start);
    Arguments rest;
    if (space == std::string::npos || !ReadArguments(text.substr(space, close - space), rest) || rest.values.size() < 3) return Color{};
    const double a = rest.hasAlpha ? Clamp01(rest.alpha) : 1;
    const std::string space_name = text.substr(start, space - start);
    const auto num = [&](size_t k) { return rest.percent[k] ? rest.values[k] / 100 : rest.values[k]; };
    if (space_name == "srgb-linear") return FromLinear(num(0), num(1), num(2), a);
    Color c;
    c.r = Byte(num(0));
    c.g = Byte(num(1));
    c.b = Byte(num(2));
    c.a = Byte(a);
    return c;
  }
  Arguments args;
  if (!ReadArguments(text.substr(open + 1, close - open - 1), args)) return Color{};
  const double alpha = args.hasAlpha ? Clamp01(args.alpha) : 1;
  const std::vector<double>& v = args.values;
  const auto at = [&](size_t i, double percentOf) { return i < v.size() ? (args.percent[i] ? v[i] * percentOf / 100 : v[i]) : 0.0; };
  if (name == "rgb" || name == "rgba") {
    if (v.size() < 3) return Color{};
    double a = alpha;
    if (v.size() >= 4 && !args.hasAlpha) a = Clamp01(args.percent[3] ? v[3] / 100 : v[3]);  // (the old form has the alpha as a fourth number)
    Color c;
    c.r = Byte(at(0, 255) / 255);
    c.g = Byte(at(1, 255) / 255);
    c.b = Byte(at(2, 255) / 255);
    c.a = Byte(a);
    return c;
  }
  if (name == "oklab" && v.size() >= 3) return FromOklab(at(0, 1), at(1, 0.4), at(2, 0.4), alpha);
  if (name == "oklch" && v.size() >= 3) {
    const double chroma = at(1, 0.4), hue = at(2, 360) * M_PI / 180;
    return FromOklab(at(0, 1), chroma * std::cos(hue), chroma * std::sin(hue), alpha);
  }
  if (name == "lab" && v.size() >= 3) return FromLab(at(0, 100), at(1, 125), at(2, 125), alpha);
  if (name == "lch" && v.size() >= 3) {
    const double chroma = at(1, 150), hue = at(2, 360) * M_PI / 180;
    return FromLab(at(0, 100), chroma * std::cos(hue), chroma * std::sin(hue), alpha);
  }
  return Color{};
}

}  // namespace solar::paint
