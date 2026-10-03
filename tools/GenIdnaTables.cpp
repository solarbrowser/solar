// Usage: GenIdnaTables <dir> > src/url/IdnaTables.cpp
// <dir> holds IdnaMappingTable.txt, UnicodeData.txt, DerivedNormalizationProps.txt,
// DerivedJoiningType.txt and DerivedBidiClass.txt of one Unicode version.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Range {
  uint32_t first;
  uint32_t last;
};

[[noreturn]] void Die(const std::string& message) {
  std::fprintf(stderr, "GenIdnaTables: %s\n", message.c_str());
  std::exit(1);
}

std::string Trim(const std::string& s) {
  size_t begin = s.find_first_not_of(" \t\r");
  if (begin == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r");
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string> Split(const std::string& s, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == separator) {
      parts.push_back(Trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  return parts;
}

uint32_t Hex(const std::string& s) { return static_cast<uint32_t>(std::stoul(s, nullptr, 16)); }

Range ParseRange(const std::string& s) {
  size_t dots = s.find("..");
  if (dots == std::string::npos) return {Hex(s), Hex(s)};
  return {Hex(s.substr(0, dots)), Hex(s.substr(dots + 2))};
}

// Each element is the ';'-separated fields of a data line with the comment removed.
std::vector<std::vector<std::string>> ReadFields(const std::string& path) {
  std::ifstream file(path);
  if (!file) Die("cannot open " + path);
  std::vector<std::vector<std::string>> rows;
  std::string line;
  while (std::getline(file, line)) {
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    if (Trim(line).empty()) continue;
    rows.push_back(Split(line, ';'));
  }
  return rows;
}

// Joins neighbouring entries that touch and carry the same value.
template <typename Entry, typename SameValue>
std::vector<Entry> MergeAdjacent(std::vector<Entry> entries, SameValue sameValue) {
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.first < b.first; });
  std::vector<Entry> merged;
  for (const Entry& entry : entries) {
    if (!merged.empty() && merged.back().last + 1 == entry.first && sameValue(merged.back(), entry)) {
      merged.back().last = entry.last;
    } else {
      merged.push_back(entry);
    }
  }
  return merged;
}

class Printer {
 public:
  void Begin(const std::string& declaration) {
    std::printf("%s = {\n", declaration.c_str());
    column_ = 0;
  }

  void Item(const std::string& item) {
    if (column_ == 0) std::printf("   ");
    std::printf(" %s,", item.c_str());
    column_ += item.size() + 2;
    if (column_ > 90) {
      std::printf("\n");
      column_ = 0;
    }
  }

  void End() {
    if (column_ != 0) std::printf("\n");
    std::printf("};\n");
  }

 private:
  size_t column_ = 0;
};

std::string Cp(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "0x%X", value);
  return buffer;
}

struct MappingEntry {
  uint32_t first;
  uint32_t last;
  int status;  // index into kStatusNames
  std::vector<uint32_t> mapping;
};

constexpr const char* kStatusNames[] = {"Valid", "Mapped", "Ignored", "Disallowed"};

void EmitMapping(const std::string& dir) {
  std::vector<MappingEntry> entries;
  for (const auto& fields : ReadFields(dir + "/IdnaMappingTable.txt")) {
    Range range = ParseRange(fields[0]);
    const std::string& status = fields[1];
    MappingEntry entry{range.first, range.last, 0, {}};

    // The URL Standard runs UTS46 with UseSTD3ASCIIRules false and Transitional_Processing
    // false, which makes the STD3 statuses behave as their plain counterparts and a
    // deviation code point stay itself.
    if (status == "valid" || status == "deviation" || status == "disallowed_STD3_valid") {
      entry.status = 0;
    } else if (status == "mapped" || status == "disallowed_STD3_mapped") {
      entry.status = 1;
      for (const std::string& part : Split(fields[2], ' ')) {
        if (!part.empty()) entry.mapping.push_back(Hex(part));
      }
    } else if (status == "ignored") {
      entry.status = 2;
    } else if (status == "disallowed") {
      entry.status = 3;
    } else {
      Die("unknown IDNA status " + status);
    }
    entries.push_back(std::move(entry));
  }

  entries = MergeAdjacent(std::move(entries), [](const MappingEntry& a, const MappingEntry& b) {
    return a.status == b.status && a.mapping == b.mapping;
  });

  std::vector<uint32_t> pool;
  std::map<std::vector<uint32_t>, uint32_t> offsets;

  Printer table;
  table.Begin("const MappingRange kMapping[]");
  for (const MappingEntry& entry : entries) {
    uint32_t offset = 0;
    if (!entry.mapping.empty()) {
      auto [it, inserted] = offsets.try_emplace(entry.mapping, static_cast<uint32_t>(pool.size()));
      if (inserted) pool.insert(pool.end(), entry.mapping.begin(), entry.mapping.end());
      offset = it->second;
    }
    table.Item("{" + Cp(entry.first) + ", " + Cp(entry.last) + ", Status::" + kStatusNames[entry.status] +
               ", " + std::to_string(entry.mapping.size()) + ", " + std::to_string(offset) + "}");
  }
  table.End();
  std::printf("const size_t kMappingCount = %zu;\n\n", entries.size());

  Printer poolPrinter;
  poolPrinter.Begin("const char32_t kMappingPool[]");
  for (uint32_t cp : pool) poolPrinter.Item(Cp(cp));
  poolPrinter.End();
  std::printf("\n");
}

void ExpandDecomposition(uint32_t cp, const std::map<uint32_t, std::vector<uint32_t>>& direct,
                         std::vector<uint32_t>& out) {
  auto it = direct.find(cp);
  if (it == direct.end()) {
    out.push_back(cp);
    return;
  }
  for (uint32_t part : it->second) ExpandDecomposition(part, direct, out);
}

void EmitNormalization(const std::string& dir) {
  std::map<uint32_t, std::vector<uint32_t>> direct;
  struct IntSpan { uint32_t first; uint32_t last; int ccc; };
  std::vector<IntSpan> ccc;
  std::vector<IntSpan> marks;

  for (const auto& fields : ReadFields(dir + "/UnicodeData.txt")) {
    uint32_t cp = Hex(fields[0]);
    const std::string& category = fields[2];
    int combining = std::stoi(fields[3]);
    const std::string& decomposition = fields[5];

    if (combining != 0) ccc.push_back({cp, cp, combining});
    if (category == "Mn" || category == "Mc" || category == "Me") marks.push_back({cp, cp, 0});

    if (!decomposition.empty() && decomposition[0] != '<') {
      std::vector<uint32_t> parts;
      for (const std::string& part : Split(decomposition, ' ')) parts.push_back(Hex(part));
      direct[cp] = std::move(parts);
    }
  }

  std::set<uint32_t> excluded;
  for (const auto& fields : ReadFields(dir + "/DerivedNormalizationProps.txt")) {
    if (fields.size() < 2 || fields[1] != "Full_Composition_Exclusion") continue;
    Range range = ParseRange(fields[0]);
    for (uint32_t cp = range.first; cp <= range.last; ++cp) excluded.insert(cp);
  }

  auto sameCcc = [](const IntSpan& a, const IntSpan& b) { return a.ccc == b.ccc; };
  ccc = MergeAdjacent(std::move(ccc), sameCcc);
  marks = MergeAdjacent(std::move(marks), sameCcc);

  Printer decompositions;
  std::vector<uint32_t> pool;
  decompositions.Begin("const Decomposition kDecompositions[]");
  for (const auto& [cp, parts] : direct) {
    std::vector<uint32_t> full;
    for (uint32_t part : parts) ExpandDecomposition(part, direct, full);
    decompositions.Item("{" + Cp(cp) + ", " + std::to_string(full.size()) + ", " + std::to_string(pool.size()) +
                        "}");
    pool.insert(pool.end(), full.begin(), full.end());
  }
  decompositions.End();
  std::printf("const size_t kDecompositionCount = %zu;\n\n", direct.size());

  Printer poolPrinter;
  poolPrinter.Begin("const char32_t kDecompositionPool[]");
  for (uint32_t cp : pool) poolPrinter.Item(Cp(cp));
  poolPrinter.End();
  std::printf("\n");

  Printer cccPrinter;
  cccPrinter.Begin("const CccRange kCcc[]");
  for (const IntSpan& entry : ccc) {
    cccPrinter.Item("{" + Cp(entry.first) + ", " + Cp(entry.last) + ", " + std::to_string(entry.ccc) + "}");
  }
  cccPrinter.End();
  std::printf("const size_t kCccCount = %zu;\n\n", ccc.size());

  struct Pair { uint32_t first, second, composite; };
  std::vector<Pair> pairs;
  for (const auto& [cp, parts] : direct) {
    if (parts.size() == 2 && !excluded.count(cp)) pairs.push_back({parts[0], parts[1], cp});
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
    return a.first != b.first ? a.first < b.first : a.second < b.second;
  });
  Printer pairPrinter;
  pairPrinter.Begin("const CompositionPair kCompositions[]");
  for (const Pair& pair : pairs) {
    pairPrinter.Item("{" + Cp(pair.first) + ", " + Cp(pair.second) + ", " + Cp(pair.composite) + "}");
  }
  pairPrinter.End();
  std::printf("const size_t kCompositionCount = %zu;\n\n", pairs.size());

  Printer markPrinter;
  markPrinter.Begin("const CodePointRange kMarks[]");
  for (const IntSpan& entry : marks) markPrinter.Item("{" + Cp(entry.first) + ", " + Cp(entry.last) + "}");
  markPrinter.End();
  std::printf("const size_t kMarkCount = %zu;\n\n", marks.size());
}

struct ValueEntry {
  uint32_t first;
  uint32_t last;
  std::string value;
};

void EmitValueTable(const std::string& path, const std::string& declaration, const std::string& countName,
                    const std::string& enumName, const std::set<std::string>& wanted) {
  std::vector<ValueEntry> entries;
  for (const auto& fields : ReadFields(path)) {
    if (fields.size() < 2 || !wanted.count(fields[1])) continue;
    Range range = ParseRange(fields[0]);
    entries.push_back({range.first, range.last, fields[1]});
  }
  entries = MergeAdjacent(std::move(entries),
                          [](const ValueEntry& a, const ValueEntry& b) { return a.value == b.value; });

  Printer printer;
  printer.Begin(declaration);
  for (const ValueEntry& entry : entries) {
    printer.Item("{" + Cp(entry.first) + ", " + Cp(entry.last) + ", " + enumName + "::" + entry.value + "}");
  }
  printer.End();
  std::printf("const size_t %s = %zu;\n\n", countName.c_str(), entries.size());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) Die("usage: GenIdnaTables <dir>");
  std::string dir = argv[1];

  std::printf("// Generated by tools/GenIdnaTables.cpp. Do not edit.\n");
  std::printf("#include \"solar/url/IdnaTables.h\"\n\nnamespace solar::url::idna {\n\n");
  EmitMapping(dir);
  EmitNormalization(dir);

  // Only these four matter to the ContextJ rules; every other joining type behaves like U.
  EmitValueTable(dir + "/DerivedJoiningType.txt", "const JoiningRange kJoining[]", "kJoiningCount",
                 "JoiningType", {"L", "R", "D", "T"});

  // Code points the file leaves out default to L, which is all the bidi rules need: the
  // other defaults cover unassigned code points, and the mapping table disallows those.
  EmitValueTable(dir + "/DerivedBidiClass.txt", "const BidiRange kBidi[]", "kBidiCount", "BidiClass",
                 {"L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON", "LRE",
                  "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"});

  std::printf("}  // namespace solar::url::idna\n");
  return 0;
}
