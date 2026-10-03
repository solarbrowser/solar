#pragma once

#include <cstddef>
#include <cstdint>

// Declarations for the tables in IdnaTables.cpp, which tools/GenIdnaTables.cpp
// generates. Every table is sorted by code point so lookups can binary search.
namespace solar::url::idna {

enum class Status : uint8_t { Valid, Mapped, Ignored, Disallowed };

struct MappingRange {
  char32_t first;
  char32_t last;
  Status status;
  uint8_t mappingLength;
  uint32_t mappingOffset;
};

struct Decomposition {
  char32_t codePoint;
  uint8_t length;
  uint32_t offset;
};

struct CccRange {
  char32_t first;
  char32_t last;
  uint8_t ccc;
};

struct CompositionPair {
  char32_t first;
  char32_t second;
  char32_t composite;
};

struct CodePointRange {
  char32_t first;
  char32_t last;
};

enum class JoiningType : uint8_t { L, R, D, T };

struct JoiningRange {
  char32_t first;
  char32_t last;
  JoiningType type;
};

enum class BidiClass : uint8_t {
  L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON,
  LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI,
};

struct BidiRange {
  char32_t first;
  char32_t last;
  BidiClass cls;
};

extern const MappingRange kMapping[];
extern const size_t kMappingCount;
extern const char32_t kMappingPool[];

extern const Decomposition kDecompositions[];
extern const size_t kDecompositionCount;
extern const char32_t kDecompositionPool[];

extern const CccRange kCcc[];
extern const size_t kCccCount;

extern const CompositionPair kCompositions[];
extern const size_t kCompositionCount;

extern const CodePointRange kMarks[];
extern const size_t kMarkCount;

extern const JoiningRange kJoining[];
extern const size_t kJoiningCount;

extern const BidiRange kBidi[];
extern const size_t kBidiCount;

}  // namespace solar::url::idna
