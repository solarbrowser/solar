#include "solar/url/Idna.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "IdnaTables.h"

namespace solar::url {

namespace {

using namespace idna;

constexpr char32_t kHangulSBase = 0xAC00;
constexpr char32_t kHangulLBase = 0x1100;
constexpr char32_t kHangulVBase = 0x1161;
constexpr char32_t kHangulTBase = 0x11A7;
constexpr char32_t kHangulLCount = 19;
constexpr char32_t kHangulVCount = 21;
constexpr char32_t kHangulTCount = 28;
constexpr char32_t kHangulNCount = kHangulVCount * kHangulTCount;
constexpr char32_t kHangulSCount = kHangulLCount * kHangulNCount;

template <typename Range>
const Range* FindRange(const Range* table, size_t count, char32_t codePoint) {
  const Range* end = table + count;
  const Range* it = std::upper_bound(table, end, codePoint,
                                     [](char32_t cp, const Range& range) { return cp < range.first; });
  if (it == table) return nullptr;
  --it;
  return codePoint <= it->last ? it : nullptr;
}

const MappingRange& LookupMapping(char32_t codePoint) {
  // kMapping covers U+0000..U+10FFFF without gaps, and callers pass scalar values only.
  return *FindRange(kMapping, kMappingCount, codePoint);
}

uint8_t CombiningClass(char32_t codePoint) {
  const CccRange* range = FindRange(kCcc, kCccCount, codePoint);
  return range ? range->ccc : 0;
}

bool IsMark(char32_t codePoint) { return FindRange(kMarks, kMarkCount, codePoint) != nullptr; }

std::optional<JoiningType> GetJoiningType(char32_t codePoint) {
  const JoiningRange* range = FindRange(kJoining, kJoiningCount, codePoint);
  if (!range) return std::nullopt;
  return range->type;
}

BidiClass GetBidiClass(char32_t codePoint) {
  const BidiRange* range = FindRange(kBidi, kBidiCount, codePoint);
  return range ? range->cls : BidiClass::L;
}

void Decompose(char32_t codePoint, std::u32string& out) {
  if (codePoint >= kHangulSBase && codePoint < kHangulSBase + kHangulSCount) {
    char32_t index = codePoint - kHangulSBase;
    out.push_back(kHangulLBase + index / kHangulNCount);
    out.push_back(kHangulVBase + (index % kHangulNCount) / kHangulTCount);
    if (index % kHangulTCount != 0) out.push_back(kHangulTBase + index % kHangulTCount);
    return;
  }

  const Decomposition* it = std::lower_bound(
      kDecompositions, kDecompositions + kDecompositionCount, codePoint,
      [](const Decomposition& entry, char32_t cp) { return entry.codePoint < cp; });
  if (it != kDecompositions + kDecompositionCount && it->codePoint == codePoint) {
    out.append(kDecompositionPool + it->offset, it->length);
  } else {
    out.push_back(codePoint);
  }
}

char32_t Compose(char32_t first, char32_t second) {
  if (first >= kHangulLBase && first < kHangulLBase + kHangulLCount && second >= kHangulVBase &&
      second < kHangulVBase + kHangulVCount) {
    return kHangulSBase + ((first - kHangulLBase) * kHangulVCount + (second - kHangulVBase)) * kHangulTCount;
  }
  if (first >= kHangulSBase && first < kHangulSBase + kHangulSCount &&
      (first - kHangulSBase) % kHangulTCount == 0 && second > kHangulTBase &&
      second < kHangulTBase + kHangulTCount) {
    return first + (second - kHangulTBase);
  }

  const CompositionPair* it = std::lower_bound(
      kCompositions, kCompositions + kCompositionCount, std::pair{first, second},
      [](const CompositionPair& entry, const std::pair<char32_t, char32_t>& key) {
        return entry.first != key.first ? entry.first < key.first : entry.second < key.second;
      });
  if (it != kCompositions + kCompositionCount && it->first == first && it->second == second) {
    return it->composite;
  }
  return 0;
}

std::u32string NormalizeNfc(std::u32string_view input) {
  std::u32string decomposed;
  decomposed.reserve(input.size());
  for (char32_t cp : input) Decompose(cp, decomposed);

  for (size_t i = 0; i < decomposed.size();) {
    if (CombiningClass(decomposed[i]) == 0) {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < decomposed.size() && CombiningClass(decomposed[end]) != 0) ++end;
    std::stable_sort(decomposed.begin() + i, decomposed.begin() + end,
                     [](char32_t a, char32_t b) { return CombiningClass(a) < CombiningClass(b); });
    i = end;
  }

  std::u32string out;
  out.reserve(decomposed.size());
  size_t starter = std::u32string::npos;
  int lastClass = 0;
  for (char32_t cp : decomposed) {
    int cls = CombiningClass(cp);
    if (starter != std::u32string::npos) {
      bool adjacent = out.size() - 1 == starter;
      // Blocked when something between the starter and cp has class 0 or a class not below cp's.
      bool blocked = !adjacent && (lastClass == 0 || lastClass >= cls);
      if (!blocked) {
        char32_t composite = Compose(out[starter], cp);
        if (composite != 0) {
          out[starter] = composite;
          continue;
        }
      }
    }
    if (cls == 0) starter = out.size();
    out.push_back(cp);
    lastClass = cls;
  }
  return out;
}

constexpr uint32_t kPunyBase = 36;
constexpr uint32_t kPunyTMin = 1;
constexpr uint32_t kPunyTMax = 26;
constexpr uint32_t kPunySkew = 38;
constexpr uint32_t kPunyDamp = 700;
constexpr uint32_t kPunyInitialBias = 72;
constexpr uint32_t kPunyInitialN = 128;

uint32_t AdaptBias(uint32_t delta, uint32_t pointCount, bool firstTime) {
  delta = firstTime ? delta / kPunyDamp : delta / 2;
  delta += delta / pointCount;
  uint32_t k = 0;
  while (delta > ((kPunyBase - kPunyTMin) * kPunyTMax) / 2) {
    delta /= kPunyBase - kPunyTMin;
    k += kPunyBase;
  }
  return k + (kPunyBase - kPunyTMin + 1) * delta / (delta + kPunySkew);
}

uint32_t Threshold(uint32_t k, uint32_t bias) {
  if (k <= bias) return kPunyTMin;
  if (k >= bias + kPunyTMax) return kPunyTMax;
  return k - bias;
}

std::optional<std::u32string> PunycodeDecode(std::u32string_view input) {
  std::u32string output;
  size_t delimiter = input.rfind(U'-');
  size_t position = 0;
  if (delimiter != std::u32string_view::npos) {
    output.assign(input.substr(0, delimiter));
    position = delimiter + 1;
  }

  uint32_t n = kPunyInitialN;
  uint32_t i = 0;
  uint32_t bias = kPunyInitialBias;

  while (position < input.size()) {
    uint32_t oldI = i;
    uint32_t weight = 1;
    for (uint32_t k = kPunyBase;; k += kPunyBase) {
      if (position >= input.size()) return std::nullopt;
      char32_t c = input[position++];
      uint32_t digit;
      if (c >= U'0' && c <= U'9') {
        digit = c - U'0' + 26;
      } else if (c >= U'a' && c <= U'z') {
        digit = c - U'a';
      } else if (c >= U'A' && c <= U'Z') {
        digit = c - U'A';
      } else {
        return std::nullopt;
      }

      if (digit > (UINT32_MAX - i) / weight) return std::nullopt;
      i += digit * weight;
      uint32_t t = Threshold(k, bias);
      if (digit < t) break;
      if (weight > UINT32_MAX / (kPunyBase - t)) return std::nullopt;
      weight *= kPunyBase - t;
    }

    uint32_t length = static_cast<uint32_t>(output.size()) + 1;
    bias = AdaptBias(i - oldI, length, oldI == 0);
    if (i / length > UINT32_MAX - n) return std::nullopt;
    n += i / length;
    i %= length;
    if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) return std::nullopt;
    output.insert(output.begin() + i, static_cast<char32_t>(n));
    ++i;
  }
  return output;
}

std::optional<std::string> PunycodeEncode(std::u32string_view input) {
  std::string output;
  for (char32_t c : input) {
    if (c < 0x80) output.push_back(static_cast<char>(c));
  }
  uint32_t basicCount = static_cast<uint32_t>(output.size());
  uint32_t handled = basicCount;
  if (basicCount > 0) output.push_back('-');

  uint32_t n = kPunyInitialN;
  uint32_t delta = 0;
  uint32_t bias = kPunyInitialBias;

  auto digitChar = [](uint32_t digit) { return static_cast<char>(digit < 26 ? 'a' + digit : '0' + digit - 26); };

  while (handled < input.size()) {
    uint32_t next = UINT32_MAX;
    for (char32_t c : input) {
      if (c >= n && c < next) next = c;
    }
    if ((next - n) > (UINT32_MAX - delta) / (handled + 1)) return std::nullopt;
    delta += (next - n) * (handled + 1);
    n = next;

    for (char32_t c : input) {
      if (c < n && ++delta == 0) return std::nullopt;
      if (c != n) continue;

      uint32_t q = delta;
      for (uint32_t k = kPunyBase;; k += kPunyBase) {
        uint32_t t = Threshold(k, bias);
        if (q < t) break;
        output.push_back(digitChar(t + (q - t) % (kPunyBase - t)));
        q = (q - t) / (kPunyBase - t);
      }
      output.push_back(digitChar(q));
      bias = AdaptBias(delta, handled + 1, handled == basicCount);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  return output;
}

bool IsAscii(std::u32string_view s) {
  return std::all_of(s.begin(), s.end(), [](char32_t c) { return c < 0x80; });
}

bool StartsWithXn(std::u32string_view label) { return label.starts_with(U"xn--"); }

bool SatisfiesContextJ(std::u32string_view label) {
  for (size_t i = 0; i < label.size(); ++i) {
    char32_t c = label[i];
    if (c != 0x200C && c != 0x200D) continue;

    if (i > 0 && CombiningClass(label[i - 1]) == 9) continue;
    if (c == 0x200D) return false;

    size_t left = i;
    while (left > 0 && GetJoiningType(label[left - 1]) == JoiningType::T) --left;
    if (left == 0) return false;
    std::optional<JoiningType> before = GetJoiningType(label[left - 1]);
    if (before != JoiningType::L && before != JoiningType::D) return false;

    size_t right = i + 1;
    while (right < label.size() && GetJoiningType(label[right]) == JoiningType::T) ++right;
    if (right == label.size()) return false;
    std::optional<JoiningType> after = GetJoiningType(label[right]);
    if (after != JoiningType::R && after != JoiningType::D) return false;
  }
  return true;
}

// RFC 5893 section 2, conditions 1 through 6.
bool SatisfiesBidiRule(std::u32string_view label) {
  if (label.empty()) return true;

  BidiClass first = GetBidiClass(label.front());
  bool rtl;
  if (first == BidiClass::R || first == BidiClass::AL) {
    rtl = true;
  } else if (first == BidiClass::L) {
    rtl = false;
  } else {
    return false;
  }

  bool hasEn = false;
  bool hasAn = false;
  for (char32_t c : label) {
    switch (GetBidiClass(c)) {
      case BidiClass::R: case BidiClass::AL:
        if (!rtl) return false;
        break;
      case BidiClass::L:
        if (rtl) return false;
        break;
      case BidiClass::AN:
        if (!rtl) return false;
        hasAn = true;
        break;
      case BidiClass::EN:
        hasEn = true;
        break;
      case BidiClass::ES: case BidiClass::CS: case BidiClass::ET: case BidiClass::ON:
      case BidiClass::BN: case BidiClass::NSM:
        break;
      default:
        return false;
    }
  }
  if (rtl && hasEn && hasAn) return false;

  size_t end = label.size();
  while (end > 0 && GetBidiClass(label[end - 1]) == BidiClass::NSM) --end;
  if (end == 0) return false;
  BidiClass last = GetBidiClass(label[end - 1]);
  if (rtl) return last == BidiClass::R || last == BidiClass::AL || last == BidiClass::EN || last == BidiClass::AN;
  return last == BidiClass::L || last == BidiClass::EN;
}

bool IsValidLabel(std::u32string_view label, bool checkBidi) {
  if (label.empty()) return true;
  if (NormalizeNfc(label) != label) return false;
  if (StartsWithXn(label)) return false;
  if (IsMark(label.front())) return false;
  for (char32_t c : label) {
    if (c == U'.' || LookupMapping(c).status != Status::Valid) return false;
  }
  if (!SatisfiesContextJ(label)) return false;
  return !checkBidi || SatisfiesBidiRule(label);
}

std::optional<std::string> ToAscii(std::u32string_view domain) {
  std::u32string mapped;
  for (char32_t c : domain) {
    const MappingRange& range = LookupMapping(c);
    switch (range.status) {
      case Status::Valid: mapped.push_back(c); break;
      case Status::Mapped: mapped.append(kMappingPool + range.mappingOffset, range.mappingLength); break;
      case Status::Ignored: break;
      case Status::Disallowed: return std::nullopt;
    }
  }

  std::u32string normalized = NormalizeNfc(mapped);

  std::vector<std::u32string> labels;
  size_t start = 0;
  for (size_t i = 0; i <= normalized.size(); ++i) {
    if (i == normalized.size() || normalized[i] == U'.') {
      labels.push_back(normalized.substr(start, i - start));
      start = i + 1;
    }
  }

  bool bidiDomain = false;
  for (std::u32string& label : labels) {
    if (StartsWithXn(label)) {
      std::u32string_view rest = std::u32string_view(label).substr(4);
      if (!IsAscii(rest)) return std::nullopt;
      std::optional<std::u32string> decoded = PunycodeDecode(rest);
      if (!decoded || decoded->empty() || IsAscii(*decoded)) return std::nullopt;
      label = std::move(*decoded);
    }
    for (char32_t c : label) {
      BidiClass cls = GetBidiClass(c);
      bidiDomain = bidiDomain || cls == BidiClass::R || cls == BidiClass::AL || cls == BidiClass::AN;
    }
  }

  std::string result;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (!IsValidLabel(labels[i], bidiDomain)) return std::nullopt;

    if (i > 0) result.push_back('.');
    if (IsAscii(labels[i])) {
      for (char32_t c : labels[i]) result.push_back(static_cast<char>(c));
    } else {
      std::optional<std::string> encoded = PunycodeEncode(labels[i]);
      if (!encoded) return std::nullopt;
      result += "xn--" + *encoded;
    }
  }
  return result;
}

bool IsForbiddenDomainCodePoint(char c) {
  switch (c) {
    case '\0': case '\t': case '\n': case '\r': case ' ': case '#': case '/': case ':':
    case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^':
    case '|': case '%': case 0x7F:
      return true;
    default:
      return static_cast<unsigned char>(c) <= 0x1F;
  }
}

}  // namespace

std::optional<std::string> DomainToAscii(std::u32string_view domain) {
  std::optional<std::string> result;
  if (IsAscii(domain)) {
    // Web compatibility: an ASCII domain is only lowercased and never run through UTS46,
    // so malformed xn-- labels in it are not rejected here.
    std::string lowered;
    lowered.reserve(domain.size());
    for (char32_t c : domain) {
      lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : static_cast<char>(c));
    }
    result = std::move(lowered);
  } else {
    result = ToAscii(domain);
  }

  if (!result || result->empty()) return std::nullopt;
  for (char c : *result) {
    if (IsForbiddenDomainCodePoint(c)) return std::nullopt;
  }
  return result;
}

}  // namespace solar::url
