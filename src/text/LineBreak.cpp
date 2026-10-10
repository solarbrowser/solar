// Line breaking (https://www.unicode.org/reports/tr14/), with the tailoring CSS asks for (https://drafts.csswg.org/css-text-3/#line-break-property).
#include "solar/text/Unicode.h"

namespace solar::text {

namespace {

using C = LineBreakClass;

bool Eastern(char32_t c) {
  const EastAsianWidth w = EastAsianWidthOf(c);
  return w == EastAsianWidth::F || w == EastAsianWidth::W || w == EastAsianWidth::H;
}
bool EastAsian(char32_t c) {
  const EastAsianWidth w = EastAsianWidthOf(c);
  return w == EastAsianWidth::F || w == EastAsianWidth::W || w == EastAsianWidth::A;
}

bool IsIterationMark(char32_t c) { return c == 0x3005 || c == 0x303B || c == 0x309D || c == 0x309E || c == 0x30FD || c == 0x30FE; }
bool IsHyphenLike(char32_t c) { return c == 0x2010 || c == 0x2013 || c == 0x301C || c == 0x30A0; }
bool IsCentred(char32_t c) {
  switch (c) { case 0x30FB: case 0xFF1A: case 0xFF1B: case 0xFF65: case 0x203C: case 0x2047: case 0x2048: case 0x2049: case 0xFF01: case 0xFF1F: return true; default: return false; }
}
bool IsInseparable(char32_t c) { return LineBreakOf(c) == C::IN; }

}  // namespace

std::vector<char> FindLineBreaks(std::string_view text, const LineBreakOptions& options) {
  std::vector<char> result(text.size() + 1, 0);
  std::u32string cp;
  std::vector<size_t> end;  // the byte offset after each code point
  for (size_t i = 0; i < text.size();) {
    cp += DecodeUtf8(text, i);
    end.push_back(i);
  }
  const size_t n = cp.size();
  if (n == 0) return result;
  using S = LineBreakOptions::Strictness;
  if (options.strictness == S::Anywhere) {
    // Between every grapheme cluster, whatever is in them.
    for (size_t b : GraphemeBoundaries(text)) result[b] = 1;
    return result;
  }
  // Classes, resolved as LB1 says, then tailored.
  std::vector<C> raw(n), cls(n);
  for (size_t i = 0; i < n; ++i) {
    C c = LineBreakOf(cp[i]);
    raw[i] = c;
    switch (c) {
      case C::AI: case C::SG: case C::XX: c = C::AL; break;
      case C::SA: { const GeneralCategory g = GeneralCategoryOf(cp[i]); c = (g == GeneralCategory::Mn || g == GeneralCategory::Mc) ? C::CM : C::AL; break; }
      case C::CJ: c = options.strictness == S::Strict ? C::NS : C::ID; break;
      default: break;
    }
    const bool cjk = options.japaneseOrChinese;
    if (options.strictness == S::Loose) {
      if (IsInseparable(cp[i]) || IsIterationMark(cp[i])) c = C::ID;
      if (cjk) {
        if (IsHyphenLike(cp[i])) c = C::ID;
        if (IsCentred(cp[i])) c = C::ID;
        if ((c == C::PO || c == C::PR) && EastAsian(cp[i])) c = C::ID;
      }
    } else if (options.strictness == S::Normal) {
      if (cjk && IsHyphenLike(cp[i])) c = C::ID;
    }
    cls[i] = c;
  }
  if (options.words == LineBreakOptions::Words::KeepAll) {
    for (size_t i = 0; i < n; ++i) if (cls[i] == C::ID || cls[i] == C::H2 || cls[i] == C::H3 || cls[i] == C::JL || cls[i] == C::JV || cls[i] == C::JT) {
      if (!IsPunctuation(cp[i])) cls[i] = C::AL;
    }
  } else if (options.words == LineBreakOptions::Words::BreakAll) {
    for (size_t i = 0; i < n; ++i) if (cls[i] == C::AL || cls[i] == C::HL || cls[i] == C::NU) cls[i] = C::ID;
  }
  // LB9 and LB10: combining marks and zero width joiners go with what they follow.
  std::vector<char> attached(n, 0);
  std::vector<C> eff = cls;
  for (size_t i = 0; i < n; ++i) {
    if (cls[i] != C::CM && cls[i] != C::ZWJ) continue;
    if (i > 0) {
      size_t b = i;
      while (b > 0 && attached[b - 1] == 1 && (cls[b - 1] == C::CM || cls[b - 1] == C::ZWJ)) --b;
      const C base = eff[i - 1];
      const C baseRaw = cls[i - 1];
      if (base != C::BK && base != C::CR && base != C::LF && base != C::NL && base != C::SP && base != C::ZW && !(attached[i - 1] == 0 && baseRaw == C::SP)) {
        attached[i] = 1;
        eff[i] = base;
        continue;
      }
    }
    eff[i] = C::AL;  // LB10
  }
  // A cluster's class is its base's: eff[i] already says so for attached marks.
  const auto lastNonSpace = [&](size_t i) -> long {  // before position i, skipping spaces
    long j = static_cast<long>(i) - 1;
    while (j >= 0 && eff[j] == C::SP) --j;
    return j;
  };
  const auto hasPi = [&](size_t i) { return GeneralCategoryOf(cp[i]) == GeneralCategory::Pi; };
  const auto hasPf = [&](size_t i) { return GeneralCategoryOf(cp[i]) == GeneralCategory::Pf; };

  for (size_t i = 1; i <= n; ++i) {
    bool canBreak;
    if (i == n) {
      canBreak = true;  // LB3
    } else {
      const C cur = eff[i];
      const C prev = eff[i - 1];
      const C prevRaw = cls[i - 1];
      const C curRaw = cls[i];
      const bool eot = false;
      (void)eot;
      long ps = lastNonSpace(i);  // the last non-space character before i
      const C beforeSpaces = ps >= 0 ? eff[ps] : C::XX;
      canBreak = true;  // LB31 unless a rule says otherwise
      bool decided = false;
      const auto no = [&] { canBreak = false; decided = true; };
      const auto yes = [&] { canBreak = true; decided = true; };
      do {
        if (prev == C::BK) { yes(); break; }                                                // LB4
        if (prev == C::CR && cur == C::LF) { no(); break; }                                 // LB5
        if (prev == C::CR || prev == C::LF || prev == C::NL) { yes(); break; }
        if (cur == C::BK || cur == C::CR || cur == C::LF || cur == C::NL) { no(); break; }  // LB6
        if (cur == C::SP || cur == C::ZW) { no(); break; }                                  // LB7
        if (beforeSpaces == C::ZW && ps >= 0 && !attached[ps]) { yes(); break; }            // LB8
        if (prevRaw == C::ZWJ) { no(); break; }                                             // LB8a
        if (attached[i]) { no(); break; }                                                   // LB9
        if (cur == C::WJ || prev == C::WJ) { no(); break; }                                 // LB11
        if (prev == C::GL) { no(); break; }                                                 // LB12
        if (cur == C::GL && prev != C::SP && prev != C::BA && prev != C::HY && prev != C::HH) { no(); break; }  // LB12a
        if (cur == C::CL || cur == C::CP || cur == C::EX || cur == C::SY) { no(); break; }  // LB13
        if (beforeSpaces == C::OP) { no(); break; }                                         // LB14
        // LB15a: an initial quote with what is allowed before it, then spaces
        if (ps >= 0 && eff[ps] == C::QU && hasPi(ps)) {
          long before = ps - 1;
          if (before < 0 || eff[before] == C::BK || eff[before] == C::CR || eff[before] == C::LF || eff[before] == C::NL || eff[before] == C::OP || eff[before] == C::QU ||
              eff[before] == C::GL || eff[before] == C::SP || eff[before] == C::ZW) { no(); break; }
        }
        // LB15b: a final quote followed by what closes
        if (cur == C::QU && hasPf(i)) {
          if (i + 1 >= n) { no(); break; }
          const C next = eff[i + 1];
          if (next == C::SP || next == C::GL || next == C::WJ || next == C::CL || next == C::QU || next == C::CP || next == C::EX || next == C::IS || next == C::SY || next == C::BK ||
              next == C::CR || next == C::LF || next == C::NL || next == C::ZW) { no(); break; }
        }
        if (prev == C::SP && cur == C::IS && i + 1 < n && eff[i + 1] == C::NU) { yes(); break; }  // LB15c
        if (cur == C::IS) { no(); break; }                                                  // LB15d
        if ((beforeSpaces == C::CL || beforeSpaces == C::CP) && cur == C::NS) { no(); break; }  // LB16
        if (beforeSpaces == C::B2 && cur == C::B2) { no(); break; }                         // LB17
        if (prev == C::SP) { yes(); break; }                                                // LB18
        // LB19 and 19a
        if (cur == C::QU || prev == C::QU) {
          const bool curEast = cur == C::QU ? false : false;
          (void)curEast;
          const auto east = [&](size_t k) { return Eastern(cp[k]); };
          if (cur == C::QU) {
            if (!east(i - 1)) { no(); break; }
            if (i + 1 >= n || !east(i + 1)) { no(); break; }
          }
          if (prev == C::QU) {
            if (!east(i)) { no(); break; }
            if (i - 1 == 0 || !east(i - 2)) { no(); break; }
          }
        }
        if (cur == C::CB || prev == C::CB) { yes(); break; }                                // LB20
        // LB20a: a hyphen at the start of a word with a letter after it
        if ((prev == C::HY || prev == C::HH) && (cur == C::AL || cur == C::HL)) {
          if (i - 1 == 0) { no(); break; }
          const C before = eff[i - 2];
          if (before == C::BK || before == C::CR || before == C::LF || before == C::NL || before == C::SP || before == C::ZW || before == C::CB || before == C::GL) { no(); break; }
        }
        if (options.japaneseOrChinese && options.strictness != S::Loose && (cur == C::PR || cur == C::PO) && EastAsian(cp[i])) { no(); break; }  // CSS: not before a full width prefix or postfix
        if (cur == C::BA || cur == C::HY || cur == C::NS || prev == C::BB) { no(); break; }  // LB21
        if (prev == C::HY || prev == C::HH || (prev == C::BA && !Eastern(cp[i - 1]))) {      // LB21a
          if (i >= 2 && eff[i - 2] == C::HL) { no(); break; }
        }
        if (prev == C::SY && cur == C::HL) { no(); break; }                                 // LB21b
        if (cur == C::IN) { no(); break; }                                                  // LB22
        if ((prev == C::AL || prev == C::HL) && cur == C::NU) { no(); break; }              // LB23
        if (prev == C::NU && (cur == C::AL || cur == C::HL)) { no(); break; }
        if (prev == C::PR && (cur == C::ID || cur == C::EB || cur == C::EM)) { no(); break; }  // LB23a
        if ((prev == C::ID || prev == C::EB || prev == C::EM) && cur == C::PO) { no(); break; }
        if ((prev == C::PR || prev == C::PO) && (cur == C::AL || cur == C::HL)) { no(); break; }  // LB24
        if ((prev == C::AL || prev == C::HL) && (cur == C::PR || cur == C::PO)) { no(); break; }
        // LB25
        if ((prev == C::PR || prev == C::PO) && (cur == C::NU || ((cur == C::OP || cur == C::HY) && i + 1 < n && eff[i + 1] == C::NU))) { no(); break; }
        if ((prev == C::OP || prev == C::HY) && cur == C::NU) { no(); break; }
        if (prev == C::NU && (cur == C::NU || cur == C::SY || cur == C::IS)) { no(); break; }
        {
          // NU (NU|SY|IS)* (CL|CP)? × (PO|PR), and NU (NU|SY|IS)* × (CL|CP)
          long j = static_cast<long>(i) - 1;
          if (eff[j] == C::CL || eff[j] == C::CP) {
            if (cur == C::PO || cur == C::PR) {
              long k = j - 1;
              while (k >= 0 && (eff[k] == C::NU || eff[k] == C::SY || eff[k] == C::IS)) --k;
              if (k < j - 1 && eff[k + 1] == C::NU) { /* chain found */ }
              if (k < j - 1) { bool hasNu = false; for (long m = k + 1; m < j; ++m) if (eff[m] == C::NU) hasNu = true; if (hasNu) { no(); break; } }
            }
          } else if (eff[j] == C::NU || eff[j] == C::SY || eff[j] == C::IS) {
            long k = j;
            while (k >= 0 && (eff[k] == C::NU || eff[k] == C::SY || eff[k] == C::IS)) --k;
            bool hasNu = false;
            for (long m = k + 1; m <= j; ++m) if (eff[m] == C::NU) hasNu = true;
            if (hasNu && (cur == C::PO || cur == C::PR)) { no(); break; }
            if (hasNu && (cur == C::CL || cur == C::CP)) { no(); break; }
          }
        }
        // LB26 and LB27: Korean syllables
        if (prev == C::JL && (cur == C::JL || cur == C::JV || cur == C::H2 || cur == C::H3)) { no(); break; }
        if ((prev == C::JV || prev == C::H2) && (cur == C::JV || cur == C::JT)) { no(); break; }
        if ((prev == C::JT || prev == C::H3) && cur == C::JT) { no(); break; }
        if ((prev == C::JL || prev == C::JV || prev == C::JT || prev == C::H2 || prev == C::H3) && cur == C::PO) { no(); break; }
        if (prev == C::PR && (cur == C::JL || cur == C::JV || cur == C::JT || cur == C::H2 || cur == C::H3)) { no(); break; }
        if ((prev == C::AL || prev == C::HL) && (cur == C::AL || cur == C::HL)) { no(); break; }  // LB28
        // LB28a: Brahmic orthographic syllables
        {
          const auto dotted = [&](size_t k) { return cp[k] == 0x25CC; };
          const bool curBase = cur == C::AK || cur == C::AS || dotted(i);
          const bool prevBase = prev == C::AK || prev == C::AS || dotted(i - 1);
          if (prev == C::AP && curBase) { no(); break; }
          if (prevBase && (cur == C::VF || cur == C::VI)) { no(); break; }
          if (prev == C::VI && i >= 2 && (eff[i - 2] == C::AK || eff[i - 2] == C::AS || dotted(i - 2)) && (cur == C::AK || dotted(i))) { no(); break; }
          if (prevBase && (cur == C::AK || cur == C::AS || dotted(i)) && i + 1 < n && eff[i + 1] == C::VF) { no(); break; }
        }
        if (prev == C::IS && (cur == C::AL || cur == C::HL)) { no(); break; }              // LB29
        // LB30: letters and numbers with opening and closing brackets that are not East Asian
        if ((prev == C::AL || prev == C::HL || prev == C::NU) && cur == C::OP && !Eastern(cp[i])) { no(); break; }
        if (prev == C::CP && !Eastern(cp[i - 1]) && (cur == C::AL || cur == C::HL || cur == C::NU)) { no(); break; }
        if (prev == C::RI && cur == C::RI) {                                                // LB30a
          int count = 0;
          for (long j = static_cast<long>(i) - 1; j >= 0 && eff[j] == C::RI; --j) ++count;
          canBreak = count % 2 == 0;
          decided = true;
          break;
        }
        if (prev == C::EB && cur == C::EM) { no(); break; }                                 // LB30b
        if (cur == C::EM && IsExtendedPictographic(cp[i - 1]) && GeneralCategoryOf(cp[i - 1]) == GeneralCategory::Cn) { no(); break; }
      } while (false);
      (void)decided;
    }
    if (canBreak) result[end[i - 1]] = 1;
  }
  return result;
}

}  // namespace solar::text
