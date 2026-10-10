// The bidirectional algorithm on a few paragraphs whose levels are known from UAX #9 and from how browsers lay them out.
#include <cstdio>
#include <string>

#include "solar/text/Unicode.h"

using namespace solar::text;

static int g_failed = 0;

static void Check(const char* name, const std::u32string& text, int base, const std::vector<int>& expected) {
  const BidiResult r = ResolveBidi(text, base);
  bool ok = r.levels.size() == expected.size();
  for (size_t i = 0; ok && i < expected.size(); ++i) ok = r.levels[i] == expected[i];
  if (!ok) {
    ++g_failed;
    std::printf("FAIL %s: got", name);
    for (uint8_t l : r.levels) std::printf(" %d", l);
    std::printf("\n");
  }
}

int main() {
  const std::u32string alef = U"אבג";
  Check("latin", U"abc", 2, {0, 0, 0});
  Check("hebrew", alef, 2, {1, 1, 1});
  Check("mixed ltr", U"abc " + alef, 0, {0, 0, 0, 0, 1, 1, 1});
  Check("mixed rtl", U"abc " + alef, 1, {2, 2, 2, 1, 1, 1, 1});
  Check("auto picks the first strong", alef + U" abc", 2, {1, 1, 1, 1, 2, 2, 2});
  Check("numbers in rtl", alef + U" 123", 1, {1, 1, 1, 1, 2, 2, 2});
  Check("arabic number context", U"ا 123", 0, {1, 1, 2, 2, 2});
  Check("neutral between rtl", alef + U"." + alef, 0, {1, 1, 1, 1, 1, 1, 1});
  Check("neutral between mixed uses the embedding", U"a." + alef, 0, {0, 0, 1, 1, 1});
  Check("brackets", U"a(" + alef + U")", 0, {0, 0, 1, 1, 1, 0});
  Check("rlo", U"‮abc‬", 0, {0, 1, 1, 1, 1});
  Check("lri", U"⁦" + alef + U"⁩", 1, {1, 3, 3, 3, 1});
  Check("rli in ltr", U"a⁧b⁩c", 0, {0, 0, 2, 0, 0});
  const std::vector<size_t> order = VisualOrder({0, 0, 1, 1, 1, 0});
  const std::vector<size_t> want = {0, 1, 4, 3, 2, 5};
  if (order != want) {
    ++g_failed;
    std::printf("FAIL visual order\n");
  }
  const std::vector<size_t> nested = VisualOrder({1, 1, 2, 2, 1});
  const std::vector<size_t> wantNested = {4, 2, 3, 1, 0};
  if (nested != wantNested) {
    ++g_failed;
    std::printf("FAIL nested visual order:");
    for (size_t i : nested) std::printf(" %zu", i);
    std::printf("\n");
  }
  std::printf("bidi: %s\n", g_failed ? "failed" : "ok");
  return g_failed ? 1 : 0;
}
