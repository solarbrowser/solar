// The Unicode Bidirectional Algorithm (UAX #9): the embedding levels of a paragraph, and the visual order of a line.
#include <algorithm>
#include <cstdint>

#include "solar/text/Unicode.h"

namespace solar::text {

namespace {

constexpr int kMaxDepth = 125;

bool IsIsolateInitiator(BidiClass c) { return c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::FSI; }
bool IsRemovedByX9(BidiClass c) {
  return c == BidiClass::LRE || c == BidiClass::RLE || c == BidiClass::LRO || c == BidiClass::RLO || c == BidiClass::PDF || c == BidiClass::BN;
}
bool IsNeutralOrIsolate(BidiClass c) {
  return c == BidiClass::B || c == BidiClass::S || c == BidiClass::WS || c == BidiClass::ON || c == BidiClass::FSI || c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::PDI;
}

// BD9: for each isolate initiator the index of its PDI (or the text's end), and for each PDI that of its initiator.
void MatchIsolates(const std::vector<BidiClass>& types, std::vector<long>& matchingPdi, std::vector<long>& matchingInitiator) {
  const size_t n = types.size();
  matchingPdi.assign(n, -1);
  matchingInitiator.assign(n, -1);
  std::vector<size_t> stack;
  for (size_t i = 0; i < n; ++i) {
    if (IsIsolateInitiator(types[i])) {
      stack.push_back(i);
    } else if (types[i] == BidiClass::PDI && !stack.empty()) {
      matchingPdi[stack.back()] = static_cast<long>(i);
      matchingInitiator[i] = static_cast<long>(stack.back());
      stack.pop_back();
    }
  }
  for (size_t i : stack) matchingPdi[i] = static_cast<long>(n);
}

// P2, P3: the level of the first strong character in [from, to), skipping isolates; -1 if there is none.
int FirstStrongLevel(const std::vector<BidiClass>& types, const std::vector<long>& matchingPdi, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    const BidiClass t = types[i];
    if (t == BidiClass::L) return 0;
    if (t == BidiClass::R || t == BidiClass::AL) return 1;
    if (IsIsolateInitiator(t)) {
      if (matchingPdi[i] < 0 || static_cast<size_t>(matchingPdi[i]) >= to) return -1;
      i = static_cast<size_t>(matchingPdi[i]);
    }
  }
  return -1;
}

struct Status {
  int level;
  int override_;  // 0 neutral, 1 L, 2 R
  bool isolate;
};

struct Sequence {
  std::vector<size_t> indices;
  int level = 0;
  BidiClass sos = BidiClass::L, eos = BidiClass::L;
};

char32_t CanonicalBracket(char32_t c) {
  if (c == 0x2329) return 0x3008;
  if (c == 0x232A) return 0x3009;
  return c;
}

BidiClass Strong(BidiClass t) {
  if (t == BidiClass::L) return BidiClass::L;
  if (t == BidiClass::R || t == BidiClass::EN || t == BidiClass::AN) return BidiClass::R;
  return BidiClass::ON;
}

// W1-W7, N0-N2 for one isolating run sequence, in place on `types` (the sequence's own copy).
void ResolveSequence(const std::u32string& text, const std::vector<BidiClass>& original, Sequence& seq, std::vector<BidiClass>& types) {
  const size_t m = seq.indices.size();
  std::vector<BidiClass> t(m);
  for (size_t k = 0; k < m; ++k) t[k] = types[seq.indices[k]];
  const BidiClass sos = seq.sos, eos = seq.eos;

  // W1: a nonspacing mark takes the type of the character before it.
  for (size_t k = 0; k < m; ++k) {
    if (t[k] != BidiClass::NSM) continue;
    if (k == 0) t[k] = sos;
    else if (IsIsolateInitiator(t[k - 1]) || t[k - 1] == BidiClass::PDI) t[k] = BidiClass::ON;
    else t[k] = t[k - 1];
  }
  // W2: a European number after an Arabic letter is an Arabic number.
  {
    BidiClass lastStrong = sos;
    for (size_t k = 0; k < m; ++k) {
      if (t[k] == BidiClass::L || t[k] == BidiClass::R || t[k] == BidiClass::AL) lastStrong = t[k];
      else if (t[k] == BidiClass::EN && lastStrong == BidiClass::AL) t[k] = BidiClass::AN;
    }
  }
  // W3
  for (size_t k = 0; k < m; ++k) if (t[k] == BidiClass::AL) t[k] = BidiClass::R;
  // W4
  for (size_t k = 1; k + 1 < m; ++k) {
    if (t[k] == BidiClass::ES && t[k - 1] == BidiClass::EN && t[k + 1] == BidiClass::EN) t[k] = BidiClass::EN;
    else if (t[k] == BidiClass::CS && t[k - 1] == BidiClass::EN && t[k + 1] == BidiClass::EN) t[k] = BidiClass::EN;
    else if (t[k] == BidiClass::CS && t[k - 1] == BidiClass::AN && t[k + 1] == BidiClass::AN) t[k] = BidiClass::AN;
  }
  // W5: terminators next to a European number are European numbers.
  for (size_t k = 0; k < m;) {
    if (t[k] != BidiClass::ET) {
      ++k;
      continue;
    }
    size_t end = k;
    while (end < m && t[end] == BidiClass::ET) ++end;
    const bool adjacent = (k > 0 && t[k - 1] == BidiClass::EN) || (end < m && t[end] == BidiClass::EN);
    if (adjacent) for (size_t j = k; j < end; ++j) t[j] = BidiClass::EN;
    k = end;
  }
  // W6
  for (size_t k = 0; k < m; ++k) if (t[k] == BidiClass::ES || t[k] == BidiClass::ET || t[k] == BidiClass::CS) t[k] = BidiClass::ON;
  // W7
  {
    BidiClass lastStrong = sos;
    for (size_t k = 0; k < m; ++k) {
      if (t[k] == BidiClass::L || t[k] == BidiClass::R) lastStrong = t[k];
      else if (t[k] == BidiClass::EN && lastStrong == BidiClass::L) t[k] = BidiClass::L;
    }
  }

  // N0: paired brackets.
  const BidiClass embedding = (seq.level & 1) ? BidiClass::R : BidiClass::L;
  struct Pair {
    size_t open, close;
  };
  std::vector<Pair> pairs;
  {
    struct Open {
      char32_t closing;
      size_t at;
    };
    std::vector<Open> stack;
    bool overflow = false;
    for (size_t k = 0; k < m && !overflow; ++k) {
      if (t[k] != BidiClass::ON) continue;
      const char32_t c = text[seq.indices[k]];
      const Bracket* bracket = Brackets().Find(c);
      if (!bracket) continue;
      if (bracket->type == 1) {
        if (stack.size() >= 63) {
          overflow = true;
          break;
        }
        stack.push_back({CanonicalBracket(static_cast<char32_t>(bracket->pair)), k});
      } else {
        const char32_t me = CanonicalBracket(c);
        for (size_t s = stack.size(); s-- > 0;) {
          if (stack[s].closing == me) {
            pairs.push_back({stack[s].at, k});
            stack.resize(s);
            break;
          }
        }
      }
    }
    if (overflow) pairs.clear();
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.open < b.open; });
  }
  for (const Pair& pair : pairs) {
    bool foundEmbedding = false, foundOpposite = false;
    for (size_t k = pair.open + 1; k < pair.close; ++k) {
      const BidiClass s = Strong(t[k]);
      if (s == BidiClass::ON) continue;
      if (s == embedding) {
        foundEmbedding = true;
        break;
      }
      foundOpposite = true;
    }
    BidiClass resolved = BidiClass::ON;
    if (foundEmbedding) {
      resolved = embedding;
    } else if (foundOpposite) {
      BidiClass context = sos;
      for (size_t k = pair.open; k-- > 0;) {
        const BidiClass s = Strong(t[k]);
        if (s != BidiClass::ON) {
          context = s;
          break;
        }
      }
      // The context before the pair decides: the opposite direction makes the pair that, the embedding's (or none) leaves it the embedding's.
      resolved = context;
    }
    if (resolved == BidiClass::ON) continue;
    t[pair.open] = t[pair.close] = resolved;
    // Marks after a bracket that were nonspacing marks take its direction.
    for (size_t b : {pair.open, pair.close}) {
      for (size_t k = b + 1; k < m && original[seq.indices[k]] == BidiClass::NSM; ++k) t[k] = resolved;
    }
  }

  // N1, N2: neutrals between strong characters of one direction take it, the others the embedding direction.
  for (size_t k = 0; k < m;) {
    if (!IsNeutralOrIsolate(t[k])) {
      ++k;
      continue;
    }
    size_t end = k;
    while (end < m && IsNeutralOrIsolate(t[end])) ++end;
    const BidiClass before = k == 0 ? sos : Strong(t[k - 1]);
    const BidiClass after = end == m ? eos : Strong(t[end]);
    const BidiClass resolved = (before == after && before != BidiClass::ON) ? before : embedding;
    for (size_t j = k; j < end; ++j) t[j] = resolved;
    k = end;
  }
  for (size_t k = 0; k < m; ++k) types[seq.indices[k]] = t[k];
}

}  // namespace

BidiResult ResolveBidi(const std::u32string& text, int baseLevel) {
  const size_t n = text.size();
  BidiResult result;
  result.levels.assign(n, 0);
  result.explicitLevels.assign(n, 0);
  result.classes.resize(n);
  if (n == 0) return result;
  std::vector<BidiClass> original(n), types(n);
  for (size_t i = 0; i < n; ++i) {
    original[i] = BidiClassOf(text[i]);
    types[i] = original[i];
    result.classes[i] = static_cast<uint8_t>(original[i]);
  }
  std::vector<long> matchingPdi, matchingInitiator;
  MatchIsolates(original, matchingPdi, matchingInitiator);

  int paragraph = baseLevel;
  if (baseLevel == 2) {
    const int found = FirstStrongLevel(original, matchingPdi, 0, n);
    paragraph = found < 0 ? 0 : found;
  }
  result.paragraphLevel = static_cast<uint8_t>(paragraph);

  // X1-X8: explicit levels and directions.
  std::vector<Status> stack;
  stack.push_back({paragraph, 0, false});
  int overflowIsolate = 0, overflowEmbedding = 0, validIsolate = 0;
  std::vector<int> levels(n, paragraph);
  for (size_t i = 0; i < n; ++i) {
    const BidiClass t = original[i];
    Status& top = stack.back();
    switch (t) {
      case BidiClass::RLE: case BidiClass::LRE: case BidiClass::RLO: case BidiClass::LRO: {
        levels[i] = top.level;
        const bool rtl = t == BidiClass::RLE || t == BidiClass::RLO;
        const int next = rtl ? ((top.level + 1) | 1) : ((top.level + 2) & ~1);
        if (next <= kMaxDepth && overflowIsolate == 0 && overflowEmbedding == 0) {
          stack.push_back({next, t == BidiClass::RLO ? 2 : t == BidiClass::LRO ? 1 : 0, false});
        } else if (overflowIsolate == 0) {
          ++overflowEmbedding;
        }
        break;
      }
      case BidiClass::RLI: case BidiClass::LRI: case BidiClass::FSI: {
        levels[i] = top.level;
        if (top.override_ == 1) types[i] = BidiClass::L;
        else if (top.override_ == 2) types[i] = BidiClass::R;
        bool rtl = t == BidiClass::RLI;
        if (t == BidiClass::FSI) {
          const size_t end = matchingPdi[i] < 0 ? n : static_cast<size_t>(matchingPdi[i]);
          rtl = FirstStrongLevel(original, matchingPdi, i + 1, end) == 1;
        }
        const int next = rtl ? ((top.level + 1) | 1) : ((top.level + 2) & ~1);
        if (next <= kMaxDepth && overflowIsolate == 0 && overflowEmbedding == 0) {
          ++validIsolate;
          stack.push_back({next, 0, true});
        } else {
          ++overflowIsolate;
        }
        break;
      }
      case BidiClass::PDI: {
        if (overflowIsolate > 0) {
          --overflowIsolate;
        } else if (validIsolate > 0) {
          overflowEmbedding = 0;
          while (!stack.back().isolate) stack.pop_back();
          stack.pop_back();
          --validIsolate;
        }
        const Status& now = stack.back();
        levels[i] = now.level;
        if (now.override_ == 1) types[i] = BidiClass::L;
        else if (now.override_ == 2) types[i] = BidiClass::R;
        break;
      }
      case BidiClass::PDF: {
        if (overflowIsolate > 0) {
          // nothing
        } else if (overflowEmbedding > 0) {
          --overflowEmbedding;
        } else if (!stack.back().isolate && stack.size() >= 2) {
          stack.pop_back();
        }
        levels[i] = stack.back().level;
        break;
      }
      case BidiClass::B:
        levels[i] = paragraph;
        break;
      case BidiClass::BN:
        levels[i] = top.level;
        break;
      default:
        levels[i] = top.level;
        if (top.override_ == 1) types[i] = BidiClass::L;
        else if (top.override_ == 2) types[i] = BidiClass::R;
        break;
    }
  }
  for (size_t i = 0; i < n; ++i) result.explicitLevels[i] = static_cast<uint8_t>(levels[i]);

  // X9, X10: the characters that stay, in runs of one level, and the isolating run sequences they make.
  std::vector<size_t> kept;
  for (size_t i = 0; i < n; ++i) if (!IsRemovedByX9(original[i])) kept.push_back(i);
  std::vector<std::vector<size_t>> runs;
  for (size_t k = 0; k < kept.size();) {
    size_t end = k;
    while (end < kept.size() && levels[kept[end]] == levels[kept[k]]) ++end;
    runs.emplace_back(kept.begin() + static_cast<long>(k), kept.begin() + static_cast<long>(end));
    k = end;
  }
  std::vector<int> runOfFirst(n, -1);
  for (size_t r = 0; r < runs.size(); ++r) runOfFirst[runs[r].front()] = static_cast<int>(r);
  std::vector<char> usedRun(runs.size(), 0);
  std::vector<size_t> positionInKept(n, 0);
  for (size_t k = 0; k < kept.size(); ++k) positionInKept[kept[k]] = k;
  for (size_t r = 0; r < runs.size(); ++r) {
    if (usedRun[r]) continue;
    // A run that starts with the PDI of an isolate whose initiator ends another run goes on from it.
    const size_t first = runs[r].front();
    if (original[first] == BidiClass::PDI && matchingInitiator[first] >= 0) continue;
    Sequence seq;
    size_t current = r;
    for (;;) {
      usedRun[current] = 1;
      seq.indices.insert(seq.indices.end(), runs[current].begin(), runs[current].end());
      const size_t last = runs[current].back();
      if (IsIsolateInitiator(original[last]) && matchingPdi[last] >= 0 && static_cast<size_t>(matchingPdi[last]) < n) {
        const int next = runOfFirst[static_cast<size_t>(matchingPdi[last])];
        if (next >= 0) {
          current = static_cast<size_t>(next);
          continue;
        }
      }
      break;
    }
    seq.level = levels[seq.indices.front()];
    // sos and eos: the higher of this level and that of the neighbor outside the sequence.
    const size_t firstKept = positionInKept[seq.indices.front()];
    const int before = firstKept == 0 ? paragraph : levels[kept[firstKept - 1]];
    const size_t lastKept = positionInKept[seq.indices.back()];
    int after = paragraph;
    if (lastKept + 1 < kept.size() && !(IsIsolateInitiator(original[seq.indices.back()]) && matchingPdi[seq.indices.back()] >= static_cast<long>(n))) {
      after = levels[kept[lastKept + 1]];
    }
    seq.sos = (std::max(before, seq.level) & 1) ? BidiClass::R : BidiClass::L;
    seq.eos = (std::max(after, seq.level) & 1) ? BidiClass::R : BidiClass::L;
    ResolveSequence(text, original, seq, types);
  }

  // I1, I2: the implicit levels.
  for (size_t k = 0; k < kept.size(); ++k) {
    const size_t i = kept[k];
    const BidiClass t = types[i];
    int level = levels[i];
    if ((level & 1) == 0) {
      if (t == BidiClass::R) level += 1;
      else if (t == BidiClass::AN || t == BidiClass::EN) level += 2;
    } else if (t == BidiClass::L || t == BidiClass::EN || t == BidiClass::AN) {
      level += 1;
    }
    levels[i] = level;
  }
  // The characters X9 removed take the level of the one before them (or the paragraph's).
  int carry = paragraph;
  for (size_t i = 0; i < n; ++i) {
    if (IsRemovedByX9(original[i])) levels[i] = carry;
    else carry = levels[i];
  }
  for (size_t i = 0; i < n; ++i) result.levels[i] = static_cast<uint8_t>(levels[i]);
  return result;
}

std::vector<uint8_t> LineLevels(const BidiResult& paragraph, size_t from, size_t to) {
  std::vector<uint8_t> out(paragraph.levels.begin() + static_cast<long>(from), paragraph.levels.begin() + static_cast<long>(to));
  // L1: segment and paragraph separators, and the white space and isolate formatting characters before them or at the end of the line,
  // are at the paragraph level.
  const auto isWhitespace = [&](size_t i) {
    const BidiClass c = static_cast<BidiClass>(paragraph.classes[i]);
    return c == BidiClass::WS || IsIsolateInitiator(c) || c == BidiClass::PDI || IsRemovedByX9(c);
  };
  bool resetting = true;
  for (size_t i = to; i-- > from;) {
    const BidiClass c = static_cast<BidiClass>(paragraph.classes[i]);
    if (c == BidiClass::B || c == BidiClass::S) {
      out[i - from] = paragraph.paragraphLevel;
      resetting = true;
    } else if (resetting && isWhitespace(i)) {
      out[i - from] = paragraph.paragraphLevel;
    } else {
      resetting = false;
    }
  }
  return out;
}

std::vector<size_t> VisualOrder(const std::vector<uint8_t>& levels) {
  const size_t n = levels.size();
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  if (n == 0) return order;
  uint8_t highest = 0, lowestOdd = 255;
  for (uint8_t l : levels) {
    highest = std::max(highest, l);
    if ((l & 1) && l < lowestOdd) lowestOdd = l;
  }
  // L2: from the highest level down to the lowest odd one, reverse every run at that level or higher.
  for (int level = highest; level >= lowestOdd && level > 0; --level) {
    for (size_t i = 0; i < n;) {
      if (levels[order[i]] < level) {
        ++i;
        continue;
      }
      size_t end = i;
      while (end < n && levels[order[end]] >= level) ++end;
      std::reverse(order.begin() + static_cast<long>(i), order.begin() + static_cast<long>(end));
      i = end;
    }
  }
  return order;
}

}  // namespace solar::text
