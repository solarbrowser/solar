// Counter styles (https://www.w3.org/TR/css-counter-styles-3/): the numbering of list markers and of counter(), from the styles the
// standard defines and the @counter-style rules of the document.
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <sstream>

#include "Internal.h"
#include "solar/css/Cssom.h"
#include "solar/css/Style.h"
#include "solar/css/Syntax.h"
#include "solar/css/Tokenizer.h"

namespace solar::layout {

namespace {

using solar::css::ComponentValue;
using solar::css::ComponentValues;
using T = solar::css::Token::Type;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Glyphs(const std::string& text) {
  std::vector<std::string> out;
  for (size_t i = 0; i < text.size();) {
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    out.push_back(text.substr(i, n));
    i += n;
  }
  return out;
}

std::string Encode(char32_t c) {
  std::string s;
  solar::css::AppendUtf8(s, c);
  return s;
}

}  // namespace

namespace {

std::map<std::string, CounterStyleDef>& Predefined() {
  static std::map<std::string, CounterStyleDef> table = [] {
    std::map<std::string, CounterStyleDef> t;
    const auto numeric = [&](const char* name, char32_t zero) {
      CounterStyleDef d;
      d.system = "numeric";
      for (int i = 0; i < 10; ++i) d.symbols.push_back(Encode(zero + i));
      t[name] = d;
    };
    const auto symbolsOf = [](const std::string& list) {
      std::vector<std::string> v;
      for (const std::string& g : Glyphs(list)) if (g != " ") v.push_back(g);
      return v;
    };
    const auto numericFrom = [&](const char* name, const std::string& list) {
      CounterStyleDef d;
      d.system = "numeric";
      d.symbols = symbolsOf(list);
      t[name] = d;
    };
    const auto alphabetic = [&](const char* name, const std::string& list, const char* suffix = ". ") {
      CounterStyleDef d;
      d.system = "alphabetic";
      d.symbols = symbolsOf(list);
      d.suffix = suffix;
      t[name] = d;
    };
    const auto cyclicSymbol = [&](const char* name, const std::string& symbol) {
      CounterStyleDef d;
      d.system = "cyclic";
      d.symbols = {symbol};
      d.suffix = " ";
      t[name] = d;
    };
    const auto additive = [&](const char* name, std::vector<std::pair<int, std::string>> weights, int lo = 1, int hi = INT_MAX) {
      CounterStyleDef d;
      d.system = "additive";
      d.additive = std::move(weights);
      d.rangeAuto = false;
      d.rangeLo = lo;
      d.rangeHi = hi;
      t[name] = d;
    };
    numeric("decimal", U'0');
    numeric("arabic-indic", 0x660); numeric("persian", 0x6F0); numeric("bengali", 0x9E6); numeric("devanagari", 0x966); numeric("gujarati", 0xAE6);
    numeric("gurmukhi", 0xA66); numeric("kannada", 0xCE6); numeric("khmer", 0x17E0); numeric("lao", 0xED0); numeric("malayalam", 0xD66);
    numeric("mongolian", 0x1810); numeric("myanmar", 0x1040); numeric("oriya", 0xB66); numeric("tamil", 0xBE6); numeric("telugu", 0xC66);
    numeric("thai", 0xE50); numeric("tibetan", 0xF20); numeric("urdu", 0x6F0);
    numeric("lower-roman-digits", U'0');
    t["cambodian"] = t["khmer"];
    t.erase("lower-roman-digits");
    numericFrom("cjk-decimal", "〇一二三四五六七八九");
    t["cjk-decimal"].suffix = "、";
    {
      CounterStyleDef d;
      d.system = "extends";
      d.extends = "decimal";
      d.padLength = 2;
      d.padSymbol = "0";
      d.hasSystem = true;
      t["decimal-leading-zero"] = d;
    }
    alphabetic("lower-alpha", "abcdefghijklmnopqrstuvwxyz");
    t["lower-latin"] = t["lower-alpha"];
    alphabetic("upper-alpha", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    t["upper-latin"] = t["upper-alpha"];
    alphabetic("lower-greek", "αβγδεζηθικλμνξοπρστυφχψω");
    alphabetic("hiragana", "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわゐゑをん", "、");
    alphabetic("hiragana-iroha", "いろはにほへとちりぬるをわかよたれそつねならむうゐのおくやまけふこえてあさきゆめみしゑひもせす", "、");
    alphabetic("katakana", "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヰヱヲン", "、");
    alphabetic("katakana-iroha", "イロハニホヘトチリヌルヲワカヨタレソツネナラムウヰノオクヤマケフコエテアサキユメミシヱヒモセス", "、");
    alphabetic("cjk-earthly-branch", "子丑寅卯辰巳午未申酉戌亥", "、");
    alphabetic("cjk-heavenly-stem", "甲乙丙丁戊己庚辛壬癸", "、");
    alphabetic("lower-armenian", "աբգդեզէըթժիլխծկհձղճմյնշոչպջռսվտրցւփքօֆ");
    alphabetic("upper-armenian", "ԱԲԳԴԵԶԷԸԹԺԻԼԽԾԿՀՁՂՃՄՅՆՇՈՉՊՋՌՍՎՏՐՑՒՓՔՕՖ");
    alphabetic("lower-russian-short", "абвгдежзиклмнопрстуфхцчшщэюя");
    t.erase("lower-russian-short");
    additive("lower-roman", {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}}, 1, 3999);
    additive("upper-roman", {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}}, 1, 3999);
    additive("hebrew", {{400, "ת"}, {300, "ש"}, {200, "ר"}, {100, "ק"}, {90, "צ"}, {80, "פ"}, {70, "ע"}, {60, "ס"}, {50, "נ"}, {40, "מ"}, {30, "ל"}, {20, "כ"}, {19, "יט"},
                         {18, "יח"}, {17, "יז"}, {16, "טז"}, {15, "טו"}, {10, "י"}, {9, "ט"}, {8, "ח"}, {7, "ז"}, {6, "ו"}, {5, "ה"}, {4, "ד"}, {3, "ג"}, {2, "ב"}, {1, "א"}}, 1, 999);
    additive("georgian", {{10000, "ჵ"}, {9000, "ჰ"}, {8000, "ჯ"}, {7000, "ჴ"}, {6000, "ხ"}, {5000, "ჭ"}, {4000, "წ"}, {3000, "ძ"}, {2000, "ც"}, {1000, "ჩ"}, {900, "შ"}, {800, "ყ"},
                           {700, "ღ"}, {600, "ქ"}, {500, "ფ"}, {400, "ჳ"}, {300, "ტ"}, {200, "ს"}, {100, "რ"}, {90, "ჟ"}, {80, "პ"}, {70, "ო"}, {60, "ჲ"}, {50, "ნ"}, {40, "მ"},
                           {30, "ლ"}, {20, "კ"}, {10, "ი"}, {9, "თ"}, {8, "ჱ"}, {7, "ზ"}, {6, "ვ"}, {5, "ე"}, {4, "დ"}, {3, "გ"}, {2, "ბ"}, {1, "ა"}}, 1, 19999);
    additive("upper-armenian", {{9000, "Ք"}, {8000, "Փ"}, {7000, "Ւ"}, {6000, "Ց"}, {5000, "Ր"}, {4000, "Տ"}, {3000, "Վ"}, {2000, "Ս"}, {1000, "Ռ"}, {900, "Ջ"}, {800, "Պ"},
                                {700, "Չ"}, {600, "Ո"}, {500, "Շ"}, {400, "Ն"}, {300, "Յ"}, {200, "Մ"}, {100, "Ճ"}, {90, "Ղ"}, {80, "Ձ"}, {70, "Հ"}, {60, "Կ"}, {50, "Ծ"}, {40, "Խ"},
                                {30, "Լ"}, {20, "Ի"}, {10, "Ժ"}, {9, "Թ"}, {8, "Ը"}, {7, "Է"}, {6, "Զ"}, {5, "Ե"}, {4, "Դ"}, {3, "Գ"}, {2, "Բ"}, {1, "Ա"}}, 1, 9999);
    additive("lower-armenian", {{9000, "ք"}, {8000, "փ"}, {7000, "ւ"}, {6000, "ց"}, {5000, "ր"}, {4000, "տ"}, {3000, "վ"}, {2000, "ս"}, {1000, "ռ"}, {900, "ջ"}, {800, "պ"},
                                {700, "չ"}, {600, "ո"}, {500, "շ"}, {400, "ն"}, {300, "յ"}, {200, "մ"}, {100, "ճ"}, {90, "ղ"}, {80, "ձ"}, {70, "հ"}, {60, "կ"}, {50, "ծ"}, {40, "խ"},
                                {30, "լ"}, {20, "ի"}, {10, "ժ"}, {9, "թ"}, {8, "ը"}, {7, "է"}, {6, "զ"}, {5, "ե"}, {4, "դ"}, {3, "գ"}, {2, "բ"}, {1, "ա"}}, 1, 9999);
    t["armenian"] = t["upper-armenian"];
    additive("cjk-ideographic", {{10000, "萬"}, {9000, "九千"}, {8000, "八千"}, {7000, "七千"}, {6000, "六千"}, {5000, "五千"}, {4000, "四千"}, {3000, "三千"}, {2000, "二千"},
                                  {1000, "一千"}, {900, "九百"}, {800, "八百"}, {700, "七百"}, {600, "六百"}, {500, "五百"}, {400, "四百"}, {300, "三百"}, {200, "二百"},
                                  {100, "一百"}, {90, "九十"}, {80, "八十"}, {70, "七十"}, {60, "六十"}, {50, "五十"}, {40, "四十"}, {30, "三十"}, {20, "二十"}, {10, "一十"},
                                  {9, "九"}, {8, "八"}, {7, "七"}, {6, "六"}, {5, "五"}, {4, "四"}, {3, "三"}, {2, "二"}, {1, "一"}, {0, "零"}}, 0, INT_MAX);
    t["cjk-ideographic"].suffix = "、";
    t["trad-chinese-informal"] = t["cjk-ideographic"];
    t["simp-chinese-informal"] = t["cjk-ideographic"];
    t["japanese-informal"] = t["cjk-ideographic"];
    cyclicSymbol("disc", "•");
    cyclicSymbol("circle", "◦");
    cyclicSymbol("square", "▪");
    cyclicSymbol("disclosure-open", "▾");
    cyclicSymbol("disclosure-closed", "▸");
    return t;
  }();
  return table;
}

// The numeric system with the range its system gives it.
bool InRange(const CounterStyleDef& d, long long value) {
  if (!d.ranges.empty()) {
    for (const auto& [lo, hi] : d.ranges) if (value >= lo && value <= hi) return true;
    return false;
  }
  if (!d.rangeAuto) return value >= d.rangeLo && value <= d.rangeHi;
  const std::string& s = d.system;
  if (s == "cyclic" || s == "numeric" || s == "fixed") return true;
  if (s == "additive") return value >= 0;
  return value >= 1;  // symbolic, alphabetic
}

}  // namespace

CounterStyles::CounterStyles(dom::Document* document) {
  if (!document) return;
  std::vector<std::string> layerOrder;
  const auto rank = [&](const std::string& path) {
    if (path.empty()) return INT_MAX;
    auto found = std::find(layerOrder.begin(), layerOrder.end(), path);
    if (found == layerOrder.end()) { layerOrder.push_back(path); return static_cast<int>(layerOrder.size()) - 1; }
    return static_cast<int>(found - layerOrder.begin());
  };
  struct Candidate {
    CounterStyleDef def;
    int layer;
    int order;
  };
  std::map<std::string, Candidate> best;
  int order = 0;
  const std::function<void(const std::vector<css::CssRule*>&, const std::string&)> walk = [&](const std::vector<css::CssRule*>& rules, const std::string& layer) {
    for (const css::CssRule* rule : rules) {
      switch (rule->kind) {
        case css::RuleKind::LayerStatement: {
          std::string names = rule->layerName;
          std::stringstream ss(names);
          std::string one;
          while (std::getline(ss, one, ',')) {
            while (!one.empty() && one.front() == ' ') one.erase(0, 1);
            if (!one.empty()) rank(layer.empty() ? one : layer + "." + one);
          }
          break;
        }
        case css::RuleKind::LayerBlock: {
          const std::string path = rule->name.empty() ? layer + "(anonymous" + std::to_string(order) + ")" : (layer.empty() ? rule->name : layer + "." + rule->name);
          rank(path);
          walk(rule->rules, path);
          break;
        }
        case css::RuleKind::Media: case css::RuleKind::Supports:
          walk(rule->rules, layer);
          break;
        case css::RuleKind::Import:
          if (rule->importedSheet) {
            const std::string path = rule->layerName.empty() ? layer : (layer.empty() ? rule->layerName : layer + "." + rule->layerName);
            if (!rule->layerName.empty()) rank(path);
            walk(rule->importedSheet->rules, path);
          }
          break;
        case css::RuleKind::CounterStyle: {
          if (!rule->style) break;
          const std::string name = Lower(rule->name);
          if (name == "none" || name == "inherit" || name == "initial" || name == "unset" || name == "default") break;
          CounterStyleDef d;
          d.hasSystem = false;
          const auto get = [&](const char* n) -> const css::DeclarationEntry* { return rule->style->Find(n); };
          if (const auto* e = get("system")) {
            const ComponentValues v = solar::css::Trimmed(solar::css::ParseComponentValues(e->value));
            if (!v.empty() && v[0].IsIdent()) {
              d.system = Lower(v[0].token.value);
              d.hasSystem = true;
              if (d.system == "extends" && v.size() >= 2) {
                for (size_t i = 1; i < v.size(); ++i) if (v[i].IsIdent()) d.extends = v[i].token.value;
              }
              if (d.system == "fixed") {
                for (size_t i = 1; i < v.size(); ++i) if (v[i].IsToken(T::Number)) d.fixedFirst = static_cast<int>(v[i].token.number);
              }
            }
          }
          const auto strings = [&](const css::DeclarationEntry* e) {
            std::vector<std::string> out;
            if (!e) return out;
            for (const ComponentValue& v : solar::css::ParseComponentValues(e->value)) {
              if (v.IsToken(T::String)) out.push_back(v.token.value);
              else if (v.IsIdent()) out.push_back(v.token.value);
            }
            return out;
          };
          if (const auto* e = get("symbols")) d.symbols = strings(e);
          if (const auto* e = get("additive-symbols")) {
            const ComponentValues v = solar::css::ParseComponentValues(e->value);
            int weight = 0;
            bool haveWeight = false;
            for (const ComponentValue& c : v) {
              if (c.IsToken(T::Number)) { weight = static_cast<int>(c.token.number); haveWeight = true; }
              else if ((c.IsToken(T::String) || c.IsIdent()) && haveWeight) { d.additive.push_back({weight, c.token.value}); haveWeight = false; }
            }
          }
          if (const auto* e = get("negative")) {
            const auto v = strings(e);
            if (!v.empty()) d.negativePrefix = v[0];
            d.negativeSuffix = v.size() > 1 ? v[1] : "";
          }
          if (const auto* e = get("prefix")) { const auto v = strings(e); d.prefix = v.empty() ? "" : v[0]; }
          if (const auto* e = get("suffix")) { const auto v = strings(e); d.suffix = v.empty() ? "" : v[0]; }
          if (const auto* e = get("fallback")) { const auto v = strings(e); if (!v.empty()) d.fallback = v[0]; }
          if (const auto* e = get("pad")) {
            for (const ComponentValue& c : solar::css::ParseComponentValues(e->value)) {
              if (c.IsToken(T::Number)) d.padLength = static_cast<int>(c.token.number);
              else if (c.IsToken(T::String)) d.padSymbol = c.token.value;
            }
          }
          if (const auto* e = get("range")) {
            if (Lower(e->value) != "auto") {
              d.rangeAuto = false;
              long long lo = 0;
              bool haveLo = false;
              for (const ComponentValue& c : solar::css::ParseComponentValues(e->value)) {
                long long v = 0;
                bool isNumber = true;
                if (c.IsToken(T::Number)) v = static_cast<long long>(c.token.number);
                else if (c.IsIdent() && Lower(c.token.value) == "infinite") v = 0;
                else isNumber = false;
                if (c.IsIdent() && Lower(c.token.value) == "infinite") v = haveLo ? LLONG_MAX : LLONG_MIN;
                if (!isNumber && !(c.IsIdent() && Lower(c.token.value) == "infinite")) continue;
                if (!haveLo) { lo = v; haveLo = true; }
                else { d.ranges.push_back({lo, v}); haveLo = false; }
              }
            }
          }
          const std::string key = name;
          const int r = rank(layer);
          auto found = best.find(key);
          const Candidate c{d, r, order++};
          if (found == best.end() || c.layer > found->second.layer || (c.layer == found->second.layer && c.order > found->second.order)) best[key] = c;
          break;
        }
        default: break;
      }
    }
  };
  for (const css::CssStyleSheet* sheet : css::SheetsOfTreeRoot(document)) if (!sheet->disabled) walk(sheet->rules, "");
  for (auto& [name, candidate] : best) defs_[name] = candidate.def;
}

const CounterStyleDef* CounterStyles::Find(const std::string& name) const {
  const std::string key = Lower(name);
  const auto found = defs_.find(key);
  if (found != defs_.end()) return &found->second;
  const auto builtin = Predefined().find(key);
  return builtin == Predefined().end() ? nullptr : &builtin->second;
}

// The representation of the value in the style, following fallbacks; false if there is none (no such style).
bool CounterStyles::Represent(const std::string& styleName, long long value, std::string& out, int depth) const {
  const CounterStyleDef* d = Find(styleName);
  if (!d || depth > 8) {
    if (depth > 8 || styleName == "decimal") { out = std::to_string(value); return true; }
    return Represent("decimal", value, out, depth + 1);
  }
  // extends: the other's system with this one's overrides
  CounterStyleDef merged = *d;
  int guard = 0;
  while (merged.system == "extends" && guard++ < 8) {
    const CounterStyleDef* base = Find(merged.extends.empty() ? "decimal" : merged.extends);
    if (!base || base == d) { merged.system = "symbolic"; break; }
    CounterStyleDef next = *base;
    if (!d->prefix.empty() || d->suffix != ". ") { next.prefix = d->prefix; next.suffix = d->suffix; }
    next.fallback = d->fallback != "decimal" ? d->fallback : next.fallback;
    if (d->padLength) { next.padLength = d->padLength; next.padSymbol = d->padSymbol; }
    if (d->negativePrefix != "-" || !d->negativeSuffix.empty()) { next.negativePrefix = d->negativePrefix; next.negativeSuffix = d->negativeSuffix; }
    if (!d->rangeAuto) { next.rangeAuto = false; next.rangeLo = d->rangeLo; next.rangeHi = d->rangeHi; next.ranges = d->ranges; }
    merged = next;
  }
  const CounterStyleDef& s = merged;
  if (!InRange(s, value)) return Represent(s.fallback, value, out, depth + 1);
  bool negative = false;
  long long magnitude = value;
  const std::string& system = s.system;
  const bool usesNegative = system == "symbolic" || system == "alphabetic" || system == "numeric" || system == "additive";
  if (usesNegative && value < 0) { negative = true; magnitude = -value; }
  std::string text;
  const size_t n = s.symbols.size();
  if (system == "cyclic") {
    if (n == 0) return Represent(s.fallback, value, out, depth + 1);
    long long index = ((value - 1) % static_cast<long long>(n) + static_cast<long long>(n)) % static_cast<long long>(n);
    text = s.symbols[index];
  } else if (system == "fixed") {
    const long long first = s.fixedFirst;
    if (value < first || value >= first + static_cast<long long>(n) || n == 0) return Represent(s.fallback, value, out, depth + 1);
    text = s.symbols[value - first];
  } else if (system == "symbolic") {
    if (n == 0 || magnitude < 1) return Represent(s.fallback, value, out, depth + 1);
    const long long index = (magnitude - 1) % static_cast<long long>(n), repeat = (magnitude - 1) / static_cast<long long>(n) + 1;
    for (long long i = 0; i < repeat && i < 1000; ++i) text += s.symbols[index];
  } else if (system == "alphabetic") {
    if (n < 2 || magnitude < 1) return Represent(s.fallback, value, out, depth + 1);
    long long v = magnitude;
    std::vector<std::string> digits;
    while (v > 0) {
      --v;
      digits.push_back(s.symbols[v % static_cast<long long>(n)]);
      v /= static_cast<long long>(n);
    }
    for (size_t i = digits.size(); i-- > 0;) text += digits[i];
  } else if (system == "numeric") {
    if (n < 2) return Represent(s.fallback, value, out, depth + 1);
    if (magnitude == 0) text = s.symbols[0];
    else {
      std::vector<std::string> digits;
      long long v = magnitude;
      while (v > 0) { digits.push_back(s.symbols[v % static_cast<long long>(n)]); v /= static_cast<long long>(n); }
      for (size_t i = digits.size(); i-- > 0;) text += digits[i];
    }
  } else if (system == "additive") {
    if (s.additive.empty()) return Represent(s.fallback, value, out, depth + 1);
    long long v = magnitude;
    if (v == 0) {
      bool zero = false;
      for (const auto& [weight, symbol] : s.additive) if (weight == 0) { text = symbol; zero = true; }
      if (!zero) return Represent(s.fallback, value, out, depth + 1);
    } else {
      auto weights = s.additive;
      std::sort(weights.begin(), weights.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (const auto& [weight, symbol] : weights) {
        if (weight <= 0) continue;
        while (v >= weight) { text += symbol; v -= weight; if (text.size() > 100000) break; }
      }
      if (v != 0) return Represent(s.fallback, value, out, depth + 1);
    }
  } else {
    return Represent(s.fallback, value, out, depth + 1);
  }
  // Padding counts characters, the negative sign included.
  size_t length = Glyphs(text).size();
  if (negative) length += Glyphs(s.negativePrefix).size() + Glyphs(s.negativeSuffix).size();
  std::string padding;
  for (int i = static_cast<int>(length); i < s.padLength; ++i) padding += s.padSymbol;
  out = negative ? s.negativePrefix + padding + text + s.negativeSuffix : padding + text;
  return true;
}

std::string CounterStyles::Format(const std::string& styleName, long long value) const {
  std::string out;
  Represent(styleName, value, out, 0);
  return out;
}

bool CounterStyles::Known(const std::string& name) const { return Find(name) != nullptr; }

std::string CounterStyles::Prefix(const std::string& name) const {
  const CounterStyleDef* d = Find(name);
  if (!d) return "";
  const CounterStyleDef* base = d;
  if (d->system == "extends" && d->prefix.empty()) base = Find(d->extends);
  return base ? base->prefix : d->prefix;
}

std::string CounterStyles::Suffix(const std::string& name) const {
  const CounterStyleDef* d = Find(name);
  if (!d) return ". ";
  if (d->system == "extends") {
    // an extending style has its own suffix only if it says one
    const CounterStyleDef* base = Find(d->extends);
    if (d->suffix == ". " && base) return base->suffix;
  }
  return d->suffix;
}

}  // namespace solar::layout
