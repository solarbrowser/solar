// Transforms (https://www.w3.org/TR/css-transforms-2/): a box's transform as a matrix, and its rectangles through the transforms around it.
#include <cmath>

#include "Internal.h"
#include "solar/css/Calc.h"
#include "solar/css/Syntax.h"

namespace solar::layout {

namespace {

using solar::css::ComponentValue;
using solar::css::ComponentValues;
using T = solar::css::Token::Type;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Matrix Translate(double x, double y, double z) {
  Matrix m;
  m.m[12] = x; m.m[13] = y; m.m[14] = z;
  return m;
}

Matrix Scale(double x, double y, double z) {
  Matrix m;
  m.m[0] = x; m.m[5] = y; m.m[10] = z;
  return m;
}

Matrix Rotate(double x, double y, double z, double angle) {
  const double len = std::sqrt(x * x + y * y + z * z);
  Matrix m;
  if (len == 0) return m;
  x /= len; y /= len; z /= len;
  const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
  m.m[0] = t * x * x + c;      m.m[1] = t * x * y + s * z;  m.m[2] = t * x * z - s * y;
  m.m[4] = t * x * y - s * z;  m.m[5] = t * y * y + c;      m.m[6] = t * y * z + s * x;
  m.m[8] = t * x * z + s * y;  m.m[9] = t * y * z - s * x;  m.m[10] = t * z * z + c;
  return m;
}

// A length or percentage of `basis`; false if it is neither.
bool LengthOf(const ComponentValue& v, double basis, double& out) {
  if (v.IsToken(T::Percentage)) { out = basis * v.token.number / 100; return true; }
  if (v.IsToken(T::Number) && v.token.number == 0) { out = 0; return true; }
  if (v.IsToken(T::Dimension)) {
    const std::string u = Lower(v.token.value);
    if (u == "px") { out = v.token.number; return true; }
    const std::optional<solar::css::MathValue> m = solar::css::EvaluateNumeric(v);
    if (m && m->kind == solar::css::MathKind::Length) { out = m->value; return true; }
  }
  if (v.kind == ComponentValue::Kind::Function) {
    std::string text = solar::css::Serialize(v);
    return EvaluateCalc(text, basis, out);
  }
  return false;
}

bool AngleOf(const ComponentValue& v, double& out) {
  if (v.IsToken(T::Number) && v.token.number == 0) { out = 0; return true; }
  if (v.IsToken(T::Dimension)) {
    const std::string u = Lower(v.token.value);
    const double n = v.token.number;
    const double pi = 3.14159265358979323846;
    if (u == "deg") { out = n * pi / 180; return true; }
    if (u == "rad") { out = n; return true; }
    if (u == "grad") { out = n * pi / 200; return true; }
    if (u == "turn") { out = n * 2 * pi; return true; }
  }
  return false;
}

std::vector<ComponentValue> Arguments(const ComponentValue& f) {
  std::vector<ComponentValue> out;
  for (const ComponentValue& c : f.children) if (!c.IsWhitespace() && !c.IsToken(T::Comma)) out.push_back(c);
  return out;
}

}  // namespace

Matrix Matrix::operator*(const Matrix& o) const {
  Matrix r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * o.m[c * 4 + k];
      r.m[c * 4 + row] = sum;
    }
  }
  return r;
}

bool Matrix::IsIdentity() const {
  for (int i = 0; i < 16; ++i) if (std::fabs(m[i] - ((i % 5 == 0) ? 1.0 : 0.0)) > 1e-12) return false;
  return true;
}

bool Matrix::Is2d() const {
  return std::fabs(m[2]) < 1e-12 && std::fabs(m[3]) < 1e-12 && std::fabs(m[6]) < 1e-12 && std::fabs(m[7]) < 1e-12 && std::fabs(m[8]) < 1e-12 && std::fabs(m[9]) < 1e-12 &&
         std::fabs(m[10] - 1) < 1e-12 && std::fabs(m[11]) < 1e-12 && std::fabs(m[14]) < 1e-12 && std::fabs(m[15] - 1) < 1e-12;
}

void Matrix::Map(double x, double y, double& ox, double& oy) const {
  const double w = m[3] * x + m[7] * y + m[15];
  ox = (m[0] * x + m[4] * y + m[12]) / (w == 0 ? 1e-12 : w);
  oy = (m[1] * x + m[5] * y + m[13]) / (w == 0 ? 1e-12 : w);
}

bool ParseTransformList(const std::string& text, double width, double height, Matrix& out) {
  out = Matrix();
  const std::string trimmed = Lower(text);
  if (trimmed == "none" || trimmed.empty()) return true;
  for (const ComponentValue& f : solar::css::ParseComponentValues(text)) {
    if (f.IsWhitespace()) continue;
    if (f.kind != ComponentValue::Kind::Function) return false;
    const std::string name = Lower(f.name);
    const std::vector<ComponentValue> a = Arguments(f);
    Matrix m;
    double x = 0, y = 0, z = 0, angle = 0;
    const auto len = [&](size_t i, double basis, double& v) { return i < a.size() && LengthOf(a[i], basis, v); };
    const auto num = [&](size_t i, double& v) {
      if (i < a.size() && a[i].IsToken(T::Number)) { v = a[i].token.number; return true; }
      return false;
    };
    if (name == "matrix" && a.size() == 6) {
      double v[6];
      for (int i = 0; i < 6; ++i) if (!num(i, v[i])) return false;
      m.m[0] = v[0]; m.m[1] = v[1]; m.m[4] = v[2]; m.m[5] = v[3]; m.m[12] = v[4]; m.m[13] = v[5];
    } else if (name == "matrix3d" && a.size() == 16) {
      for (int i = 0; i < 16; ++i) if (!num(i, m.m[i])) return false;
    } else if (name == "translate") {
      if (!len(0, width, x)) return false;
      if (a.size() > 1 && !len(1, height, y)) return false;
      m = Translate(x, y, 0);
    } else if (name == "translatex") {
      if (!len(0, width, x)) return false;
      m = Translate(x, 0, 0);
    } else if (name == "translatey") {
      if (!len(0, height, y)) return false;
      m = Translate(0, y, 0);
    } else if (name == "translatez") {
      if (!len(0, 0, z)) return false;
      m = Translate(0, 0, z);
    } else if (name == "translate3d") {
      if (!len(0, width, x) || !len(1, height, y) || !len(2, 0, z)) return false;
      m = Translate(x, y, z);
    } else if (name == "scale") {
      x = y = 1;
      if (!num(0, x)) return false;
      y = x;
      if (a.size() > 1 && !num(1, y)) return false;
      m = Scale(x, y, 1);
    } else if (name == "scalex") {
      if (!num(0, x)) return false;
      m = Scale(x, 1, 1);
    } else if (name == "scaley") {
      if (!num(0, y)) return false;
      m = Scale(1, y, 1);
    } else if (name == "scalez") {
      if (!num(0, z)) return false;
      m = Scale(1, 1, z);
    } else if (name == "scale3d") {
      if (!num(0, x) || !num(1, y) || !num(2, z)) return false;
      m = Scale(x, y, z);
    } else if (name == "rotate" || name == "rotatez") {
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      m = Rotate(0, 0, 1, angle);
    } else if (name == "rotatex") {
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      m = Rotate(1, 0, 0, angle);
    } else if (name == "rotatey") {
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      m = Rotate(0, 1, 0, angle);
    } else if (name == "rotate3d") {
      if (a.size() != 4 || !num(0, x) || !num(1, y) || !num(2, z) || !AngleOf(a[3], angle)) return false;
      m = Rotate(x, y, z, angle);
    } else if (name == "skew") {
      double ay = 0;
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      if (a.size() > 1 && !AngleOf(a[1], ay)) return false;
      m.m[4] = std::tan(angle);
      m.m[1] = std::tan(ay);
    } else if (name == "skewx") {
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      m.m[4] = std::tan(angle);
    } else if (name == "skewy") {
      if (a.empty() || !AngleOf(a[0], angle)) return false;
      m.m[1] = std::tan(angle);
    } else if (name == "perspective") {
      double d = 0;
      if (a.empty()) return false;
      if (a[0].IsIdent()) d = 0;
      else if (!len(0, 0, d)) return false;
      if (d > 0) m.m[11] = -1 / d;
    } else {
      return false;
    }
    out = out * m;
  }
  return true;
}

bool TransformOf(const Box& box, Matrix& out) {
  const BoxStyle& s = *box.style;
  const bool any = s.transform != "none" || s.translate != "none" || s.rotate != "none" || s.scale != "none";
  if (!any) return false;
  const double w = box.width, h = box.height;
  Matrix m;
  // translate, rotate and scale come before transform.
  if (s.translate != "none") {
    Matrix t;
    if (ParseTransformList("translate3d(" + [&] {
          std::string args;
          const ComponentValues v = solar::css::ParseComponentValues(s.translate);
          std::vector<std::string> parts;
          for (const ComponentValue& c : v) if (!c.IsWhitespace()) parts.push_back(solar::css::Serialize(c));
          while (parts.size() < 3) parts.push_back("0px");
          for (size_t i = 0; i < 3; ++i) args += (i ? "," : "") + parts[i];
          return args;
        }() + ")", w, h, t)) m = m * t;
  }
  if (s.rotate != "none") {
    Matrix r;
    std::string call;
    const ComponentValues v = solar::css::ParseComponentValues(s.rotate);
    std::vector<std::string> parts;
    for (const ComponentValue& c : v) if (!c.IsWhitespace()) parts.push_back(solar::css::Serialize(c));
    if (parts.size() == 1) call = "rotate(" + parts[0] + ")";
    else if (parts.size() == 2) call = std::string("rotate3d(") + (Lower(parts[0]) == "x" ? "1,0,0" : Lower(parts[0]) == "y" ? "0,1,0" : "0,0,1") + "," + parts[1] + ")";
    else if (parts.size() == 4) call = "rotate3d(" + parts[0] + "," + parts[1] + "," + parts[2] + "," + parts[3] + ")";
    if (!call.empty() && ParseTransformList(call, w, h, r)) m = m * r;
  }
  if (s.scale != "none") {
    Matrix sc;
    std::vector<std::string> parts;
    for (const ComponentValue& c : solar::css::ParseComponentValues(s.scale)) if (!c.IsWhitespace()) parts.push_back(solar::css::Serialize(c));
    while (parts.size() < 3) parts.push_back(parts.size() == 1 ? parts[0] : "1");
    if (ParseTransformList("scale3d(" + parts[0] + "," + parts[1] + "," + parts[2] + ")", w, h, sc)) m = m * sc;
  }
  Matrix list;
  if (ParseTransformList(s.transform, w, h, list)) m = m * list;
  // About the origin.
  double ox = w / 2, oy = h / 2, oz = 0;
  {
    std::vector<ComponentValue> parts;
    for (const ComponentValue& c : solar::css::ParseComponentValues(s.transformOrigin)) if (!c.IsWhitespace()) parts.push_back(c);
    if (parts.size() >= 2) {
      LengthOf(parts[0], w, ox);
      LengthOf(parts[1], h, oy);
      if (parts.size() >= 3) LengthOf(parts[2], 0, oz);
    }
  }
  out = Translate(ox, oy, oz) * m * Translate(-ox, -oy, -oz);
  return true;
}

}  // namespace solar::layout
