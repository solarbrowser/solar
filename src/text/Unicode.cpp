#include "solar/text/Unicode.h"

#include <algorithm>

namespace solar::text {

uint8_t Table::Lookup(char32_t c, uint8_t fallback) const {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (c < ranges[mid].first) hi = mid;
    else if (c > ranges[mid].last) lo = mid + 1;
    else return ranges[mid].value;
  }
  return fallback;
}

const Bracket* BracketTable::Find(char32_t c) const {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (c < entries[mid].code) hi = mid;
    else if (c > entries[mid].code) lo = mid + 1;
    else return &entries[mid];
  }
  return nullptr;
}

char32_t MappingTable::Find(char32_t c) const {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (c < entries[mid].from) hi = mid;
    else if (c > entries[mid].from) lo = mid + 1;
    else return entries[mid].to;
  }
  return 0;
}

const Special* SpecialTable::Find(char32_t c) const {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (c < entries[mid].code) hi = mid;
    else if (c > entries[mid].code) lo = mid + 1;
    else return &entries[mid];
  }
  return nullptr;
}

LineBreakClass LineBreakOf(char32_t c) { return static_cast<LineBreakClass>(LineBreakTable().Lookup(c)); }
EastAsianWidth EastAsianWidthOf(char32_t c) { return static_cast<EastAsianWidth>(EastAsianWidthTable().Lookup(c)); }
bool IsExtendedPictographic(char32_t c) { return ExtendedPictographicTable().Lookup(c, 0) != 0; }
GraphemeBreak GraphemeBreakOf(char32_t c) { return static_cast<GraphemeBreak>(GraphemeBreakTable().Lookup(c)); }
WordBreak WordBreakOf(char32_t c) { return static_cast<WordBreak>(WordBreakTable().Lookup(c)); }
BidiClass BidiClassOf(char32_t c) { return static_cast<BidiClass>(BidiClassTable().Lookup(c)); }
GeneralCategory GeneralCategoryOf(char32_t c) { return static_cast<GeneralCategory>(GeneralCategoryTable().Lookup(c)); }

char32_t MirrorOf(char32_t c) {
  const char32_t m = MirrorMap().Find(c);
  return m ? m : c;
}

// ---- UTF-8 ----

char32_t DecodeUtf8(std::string_view s, size_t& at) {
  const unsigned char c = static_cast<unsigned char>(s[at]);
  if (c < 0x80) { ++at; return c; }
  int extra;
  char32_t cp;
  if ((c >> 5) == 6) { cp = c & 0x1f; extra = 1; }
  else if ((c >> 4) == 14) { cp = c & 0x0f; extra = 2; }
  else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
  else { ++at; return 0xFFFD; }
  ++at;
  for (int i = 0; i < extra; ++i) {
    if (at >= s.size() || (static_cast<unsigned char>(s[at]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<unsigned char>(s[at]) & 0x3f);
    ++at;
  }
  return cp;
}

void AppendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) out += static_cast<char>(c);
  else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f)); }
  else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f)); }
  else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3f)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f)); }
}

std::u32string FromUtf8(std::string_view s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) out += DecodeUtf8(s, i);
  return out;
}

std::string ToUtf8(std::u32string_view s) {
  std::string out;
  for (char32_t c : s) AppendUtf8(out, c);
  return out;
}

// ---- Case ----

namespace {

bool IsCased(char32_t c) {
  const GeneralCategory g = GeneralCategoryOf(c);
  return g == GeneralCategory::Lu || g == GeneralCategory::Ll || g == GeneralCategory::Lt;
}
bool IsCaseIgnorable(char32_t c) {
  const GeneralCategory g = GeneralCategoryOf(c);
  if (g == GeneralCategory::Mn || g == GeneralCategory::Me || g == GeneralCategory::Cf || g == GeneralCategory::Lm || g == GeneralCategory::Sk) return true;
  switch (c) { case '\'': case '.': case ':': case 0xAD: case 0xB7: case 0x2018: case 0x2019: case 0x2024: case 0x2027: case 0xFE13: case 0xFE52: case 0xFE55: case 0xFF07: case 0xFF0E: case 0xFF1A: return true; default: return false; }
}

}  // namespace

std::string ChangeCase(std::string_view text, CaseMode mode, std::string_view language) {
  const std::u32string in = FromUtf8(text);
  std::string lang(language);
  for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const bool turkic = lang.rfind("tr", 0) == 0 || lang.rfind("az", 0) == 0;
  const bool lithuanian = lang.rfind("lt", 0) == 0;
  std::u32string out;
  bool startOfWord = true;
  for (size_t i = 0; i < in.size(); ++i) {
    const char32_t c = in[i];
    CaseMode use = mode;
    if (mode == CaseMode::Title) {
      const WordBreak wb = WordBreakOf(c);
      const bool wordChar = IsLetter(c) || IsNumber(c) || IsMark(c) || wb == WordBreak::ExtendNumLet;
      if (startOfWord && (IsLetter(c) || IsNumber(c))) {
        startOfWord = false;
      } else {
        out += c;
        startOfWord = !(wordChar || ((c == '\'' || c == 0x2019) && !startOfWord));
        continue;
      }
    }
    // Turkic dotted and dotless i.
    if (turkic) {
      if (use == CaseMode::Upper && c == 'i') { out += 0x130; continue; }
      if (use == CaseMode::Title && c == 'i') { out += 0x130; continue; }
      if (use == CaseMode::Lower && c == 'I') {
        if (i + 1 < in.size() && in[i + 1] == 0x307) { out += 'i'; ++i; } else out += 0x131;
        continue;
      }
      if (use == CaseMode::Lower && c == 0x130) { out += 'i'; continue; }
    }
    if (lithuanian && use == CaseMode::Lower) {
      // I, J and Į with an accent above keep a dot.
      if ((c == 'I' || c == 'J' || c == 0x12E) && i + 1 < in.size() && (in[i + 1] == 0x300 || in[i + 1] == 0x301 || in[i + 1] == 0x303)) {
        out += (c == 'I' ? U'i' : c == 'J' ? U'j' : char32_t(0x12F));
        out += 0x307;
        continue;
      }
    }
    // Final sigma.
    if (use == CaseMode::Lower && c == 0x3A3) {
      bool before = false, after = false;
      for (size_t j = i; j-- > 0;) { if (IsCaseIgnorable(in[j])) continue; before = IsCased(in[j]); break; }
      for (size_t j = i + 1; j < in.size(); ++j) { if (IsCaseIgnorable(in[j])) continue; after = IsCased(in[j]); break; }
      out += (before && !after) ? char32_t(0x3C2) : char32_t(0x3C3);
      continue;
    }
    const Special* special = SpecialCasing().Find(c);
    if (special) {
      const uint32_t* m = use == CaseMode::Upper ? special->upper : use == CaseMode::Lower ? special->lower : special->title;
      for (int k = 0; k < 3 && m[k]; ++k) out += m[k];
      continue;
    }
    const MappingTable& table = use == CaseMode::Upper ? UpperMap() : use == CaseMode::Lower ? LowerMap() : TitleMap();
    const char32_t mapped = table.Find(c);
    out += mapped ? mapped : c;
  }
  return ToUtf8(out);
}

// ---- Grapheme clusters (UAX #29, with the Indic conjunct rule) ----

namespace {

bool BreakBetween(const std::u32string& t, size_t i) {
  // between t[i-1] and t[i]
  const GraphemeBreak a = GraphemeBreakOf(t[i - 1]), b = GraphemeBreakOf(t[i]);
  if (a == GraphemeBreak::CR && b == GraphemeBreak::LF) return false;
  if (a == GraphemeBreak::Control || a == GraphemeBreak::CR || a == GraphemeBreak::LF) return true;
  if (b == GraphemeBreak::Control || b == GraphemeBreak::CR || b == GraphemeBreak::LF) return true;
  if (a == GraphemeBreak::L && (b == GraphemeBreak::L || b == GraphemeBreak::V || b == GraphemeBreak::LV || b == GraphemeBreak::LVT)) return false;
  if ((a == GraphemeBreak::LV || a == GraphemeBreak::V) && (b == GraphemeBreak::V || b == GraphemeBreak::T)) return false;
  if ((a == GraphemeBreak::LVT || a == GraphemeBreak::T) && b == GraphemeBreak::T) return false;
  if (b == GraphemeBreak::Extend || b == GraphemeBreak::ZWJ || b == GraphemeBreak::SpacingMark) return false;
  if (a == GraphemeBreak::Prepend) return false;
  // GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x Consonant
  const auto incb = [](char32_t c) { return IndicConjunctBreakTable().Lookup(c); };
  if (incb(t[i]) == 2) {
    bool linker = false;
    for (size_t j = i; j-- > 0;) {
      const uint8_t v = incb(t[j]);
      if (v == 1) { linker = true; continue; }
      if (v == 3) continue;
      if (v == 2) { if (linker) return false; }
      break;
    }
  }
  // GB11: ExtPict Extend* ZWJ x ExtPict
  if (a == GraphemeBreak::ZWJ && IsExtendedPictographic(t[i])) {
    size_t j = i - 1;
    while (j > 0 && GraphemeBreakOf(t[j - 1]) == GraphemeBreak::Extend) --j;
    if (j > 0 && IsExtendedPictographic(t[j - 1])) return false;
  }
  // GB12/13: regional indicators in pairs
  if (a == GraphemeBreak::RegionalIndicator && b == GraphemeBreak::RegionalIndicator) {
    int count = 0;
    for (size_t j = i; j-- > 0 && GraphemeBreakOf(t[j]) == GraphemeBreak::RegionalIndicator;) ++count;
    return count % 2 == 0;
  }
  return true;
}

}  // namespace

std::vector<size_t> GraphemeBoundaries(std::string_view text) {
  std::vector<size_t> ends;
  std::u32string cps;
  std::vector<size_t> offsets;  // end offset of each code point
  for (size_t i = 0; i < text.size();) {
    cps += DecodeUtf8(text, i);
    offsets.push_back(i);
  }
  for (size_t i = 1; i < cps.size(); ++i) if (BreakBetween(cps, i)) ends.push_back(offsets[i - 1]);
  if (!cps.empty()) ends.push_back(offsets.back());
  return ends;
}

size_t NextGraphemeBoundary(std::string_view text, size_t at) {
  if (at >= text.size()) return text.size();
  // Enough context before and after: a cluster is short, but regional indicators and emoji sequences need a look back.
  size_t start = at;
  int back = 0;
  while (start > 0 && back < 32) {
    --start;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
    ++back;
  }
  std::u32string cps;
  std::vector<size_t> offsets;
  size_t atIndex = 0;
  for (size_t i = start; i < text.size() && cps.size() < 100;) {
    if (i == at) atIndex = cps.size();
    cps += DecodeUtf8(text, i);
    offsets.push_back(i);
  }
  for (size_t i = atIndex + 1; i < cps.size(); ++i) if (BreakBetween(cps, i)) return offsets[i - 1];
  return cps.size() < 100 ? text.size() : offsets.back();
}

}  // namespace solar::text
