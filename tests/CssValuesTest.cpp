#include <cstdio>
#include <cstring>
#include <string>

#include "solar/css/Properties.h"
#include "solar/css/Shorthands.h"
#include "solar/css/Syntax.h"
#include "solar/css/Values.h"

// The value grammar: what each property accepts, and the text it serializes to. `CssValuesTest PROPERTY VALUE` prints
// what one value comes to.

namespace {

using namespace solar::css;

// The canonical text of `value` for `property`, or "<invalid>".
std::string Parse(const std::string& property, const std::string& value) {
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition) return "<unknown property>";
  const ComponentValues values = Trimmed(ParseComponentValues(value));
  if (IsCssWideKeyword(values)) return Serialize(values);
  if (ContainsSubstitution(values)) return SerializeValue(values);
  ValueMatch match;
  if (!MatchPropertyValue(*definition, values, match)) return "<invalid>";
  return SerializeValue(match.normalized);
}

struct Case {
  const char* property;
  const char* value;
  const char* expected;  // "<invalid>" for a value the property does not take
};

const Case kCases[] = {
    {"color", "red", "red"},
    {"color", "RED", "red"},
    {"color", "banana", "<invalid>"},
    {"color", "#ABC", "rgb(170, 187, 204)"},
    {"color", "rgb(1, 2, 3)", "rgb(1, 2, 3)"},
    {"color", "rgb(1 2 3 / 50%)", "rgba(1, 2, 3, 0.5)"},
    {"width", "10PX", "10px"},
    {"width", "-1px", "<invalid>"},
    {"width", "calc(10px + 5%)", "calc(5% + 10px)"},
    {"width", "calc(1px+2px)", "<invalid>"},
    {"width", "calc(1px * 2)", "calc(2px)"},
    {"width", "calc(1px * 2px)", "<invalid>"},
    {"width", "min(1px, 2%)", "min(1px, 2%)"},
    {"margin-left", "-5px", "-5px"},
    {"margin", "1px 2px 3px 4px", "1px 2px 3px 4px"},
    {"margin", "1px 2px 3px 4px 5px", "<invalid>"},
    {"opacity", "0.50", "0.5"},
    {"opacity", "50%", "0.5"},
    {"z-index", "1.5", "<invalid>"},
    {"z-index", "-3", "-3"},
    {"display", "BLOCK", "block"},
    {"display", "inline flow-root", "inline-block"},
    {"transition-duration", "1s, 200ms", "1s, 200ms"},
    {"aspect-ratio", "16/9", "16 / 9"},
    {"aspect-ratio", "auto 3 / 4", "auto 3 / 4"},
    {"background-image", "linear-gradient(270deg, blue 0%, red 100%)", "linear-gradient(270deg, blue 0%, red 100%)"},
    {"background-image", "linear-gradient(to right, red, blue)", "linear-gradient(to right, red, blue)"},
    {"transform", "translate(10px, 20px) rotate(45DEG)", "translate(10px, 20px) rotate(45deg)"},
    {"transform", "scale(1, 2)", "scale(1, 2)"},
    {"font-family", "Arial, \"Times New Roman\", serif", "Arial, Times New Roman, serif"},
    {"grid-template-columns", "[a] 1fr [b] 2fr", "[a] 1fr [b] 2fr"},
    {"grid-column", "1 / span 2", "1 / span 2"},
    {"content", "\"x\" counter(c)", "\"x\" counter(c)"},
    {"filter", "blur(2px) brightness(50%)", "blur(2px) brightness(50%)"},
    {"cursor", "pointer", "pointer"},
    {"border-top-width", "thin", "thin"},
    {"height", "initial", "initial"},
    {"height", "var(--x)", "var(--x)"},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "expand") {
    std::vector<Longhand> longhands;
    if (!ExpandDeclaration(argv[2], argv[3], longhands)) {
      std::printf("<invalid>\n");
      return 0;
    }
    for (const Longhand& l : longhands) std::printf("%s: %s\n", l.name.c_str(), l.value.c_str());
    return 0;
  }
  if (argc == 3) {
    std::printf("%s\n", Parse(argv[1], argv[2]).c_str());
    return 0;
  }
  int failed = 0, total = 0;
  for (const Case& test : kCases) {
    ++total;
    const std::string got = Parse(test.property, test.value);
    if (got != test.expected) {
      ++failed;
      std::printf("FAIL %s: %s\n  expected %s\n  got      %s\n", test.property, test.value, test.expected, got.c_str());
    }
  }
  std::printf("css values: %d of %d passed\n", total - failed, total);
  return failed ? 1 : 0;
}
