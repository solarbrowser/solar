#pragma once

#include <memory>
#include <string>
#include <vector>

#include "solar/dom/Node.h"
#include "solar/font/Database.h"

// The fonts of a style: what font-family and the rest ask for, matched against the @font-face rules of the document and then
// the fonts installed on the system.
namespace solar::css {

// The request a style makes: the families of font-family (names unquoted, generics lowercased), weight, stretch and style.
font::Request FontRequestFor(dom::Element* element, const std::string& pseudo = "");
// The font-relative lengths of a font at a size, in px: the x-height, the width of "0", the cap height, the width of "水", and the normal line height.
struct FontUnits {
  double ex = -1, ch = -1, cap = -1, ic = -1, lineHeight = -1;
};
// What the element's own font (or a pseudo-element's) comes to; all unknown if there is none.
FontUnits FontUnitsFor(dom::Element* element, const std::string& pseudo, double fontSize);

// The request for a family list as written (font-family's computed text), and the rest as values.
font::Request MakeFontRequest(const std::string& familyText, double weight, double width, bool italic);
// The faces for the request in the document: its @font-face fonts first, then the system's; the generic fallbacks last. Never empty
// if the system has a font at all.
std::vector<std::shared_ptr<font::Face>> FontsForRequest(dom::Document* document, const font::Request& request);
// The faces that would do for the element's text, the first family's first: more than one when later families are there for the
// characters the earlier ones do not have. Never empty: a font is found whatever is asked for, if the system has any.
std::vector<std::shared_ptr<font::Face>> FontsFor(dom::Element* element, const std::string& pseudo = "");
// The first of them: the font whose metrics the element's line boxes are made of.
std::shared_ptr<font::Face> PrimaryFont(dom::Element* element, const std::string& pseudo = "");
// A font with the character, for when none of the element's fonts has it.
std::shared_ptr<font::Face> FallbackFont(dom::Element* element, char32_t codePoint, const std::string& pseudo = "");

// What a document's @font-face rule says, as the text of its descriptors (those not written are the initial ones).
struct FaceRuleInfo {
  std::string family, src, weight, style, stretch, unicodeRange, variant, featureSettings, variationSettings, display, ascentOverride, descentOverride, lineGapOverride;
};
// The @font-face rules of the document that have a family and sources, in the order DocumentFonts has them.
std::vector<FaceRuleInfo> DocumentFaceRules(dom::Document* document);
// Loads the font of the nth of them; whether it came. (State: 0 not tried, 1 loaded, 2 failed.)
bool LoadDocumentFace(dom::Document* document, size_t index);
int DocumentFaceState(dom::Document* document, size_t index);

// A font a script made with FontFace and added to document.fonts: used for its family once it has loaded.
struct ScriptFace {
  std::string family, weight, style, stretch, unicodeRange;
  std::shared_ptr<font::Face> face;
};
void SetScriptFaces(dom::Document* document, std::vector<ScriptFace> faces);

// The @font-face fonts of the document, loaded the first time one is wanted.
font::Database& DocumentFonts(dom::Document* document);

}  // namespace solar::css
