// The natives under the CSS Font Loading API (FontFace, document.fonts), which is written in script over them.
#include <algorithm>

#include "solar/css/Cssom.h"
#include "solar/css/Descriptors.h"
#include "solar/css/Fonts.h"
#include "solar/css/Shorthands.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/font/Database.h"

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

std::vector<std::shared_ptr<font::Face>>& Handles() {
  static std::vector<std::shared_ptr<font::Face>> handles;
  return handles;
}

std::string StringArg(Context& ctx, qe::Args args, size_t index) { return index < args.size() ? qe::ToWtf8(ctx, args[index]) : std::string(); }

// __solarFontOpen(bytes): the handle of the font in the bytes, or -1 if they are not one.
Value FontOpen(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::FromInt32(-1);
  const std::optional<std::span<const uint8_t>> bytes = qe::BytesOf(args[0]);
  if (!bytes) return qe::FromInt32(-1);
  std::shared_ptr<font::Face> face = font::Face::Open(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  if (!face) return qe::FromInt32(-1);
  Handles().push_back(std::move(face));
  return qe::FromInt32(static_cast<int32_t>(Handles().size() - 1));
}

// __solarFontFetch(url): the handle of the font the address has, loaded as style sheets are, or -1.
Value FontFetch(Context& ctx, Value, qe::Args args, Value) {
  const SheetLoader& load = GetSheetLoader();
  if (!load) return qe::FromInt32(-1);
  std::optional<std::string> data = load(StringArg(ctx, args, 0));
  if (!data) return qe::FromInt32(-1);
  std::shared_ptr<font::Face> face = font::Face::Open(std::move(*data));
  if (!face) return qe::FromInt32(-1);
  Handles().push_back(std::move(face));
  return qe::FromInt32(static_cast<int32_t>(Handles().size() - 1));
}

// __solarFontLocal(name): the handle of an installed font by its full or PostScript name, or -1.
Value FontLocal(Context& ctx, Value, qe::Args args, Value) {
  std::shared_ptr<font::Face> face = font::Database::System().FindLocal(StringArg(ctx, args, 0));
  if (!face) return qe::FromInt32(-1);
  Handles().push_back(std::move(face));
  return qe::FromInt32(static_cast<int32_t>(Handles().size() - 1));
}

// __solarFontDescriptor(name, text): the descriptor's value in canonical text, or null if @font-face would not take it.
Value FontDescriptor(Context& ctx, Value, qe::Args args, Value) {
  const std::string name = StringArg(ctx, args, 0);
  const std::optional<std::string> canonical = DescriptorName(DescriptorSet::FontFace, name);
  if (!canonical) return qe::Null();
  const std::optional<std::string> value = DescriptorValue(DescriptorSet::FontFace, *canonical, ParseComponentValues(StringArg(ctx, args, 1)));
  return value ? qe::FromWtf8(ctx, *value) : qe::Null();
}

// __solarFontShorthand(text): "family names \x1f weight \x1f stretch \x1f style" for a font shorthand, or null.
Value FontShorthand(Context& ctx, Value, qe::Args args, Value) {
  std::vector<Longhand> longhands;
  if (!ExpandDeclaration("font", StringArg(ctx, args, 0), longhands)) return qe::Null();
  std::string family, weight = "400", stretch = "100%", style = "normal";
  for (const Longhand& l : longhands) {
    if (l.name == "font-family") family = l.value;
    else if (l.name == "font-weight") weight = l.value;
    else if (l.name == "font-stretch" || l.name == "font-width") stretch = l.value;
    else if (l.name == "font-style") style = l.value;
  }
  return qe::FromWtf8(ctx, family + "\x1f" + weight + "\x1f" + stretch + "\x1f" + style);
}

dom::Document* DocumentOf(Context& ctx) { return dom::AssociatedDocument(ctx); }

// __solarDocumentFontFaces(): the @font-face rules of the document, a record each, fields and records apart by control characters.
Value DocumentFaces(Context& ctx, Value, qe::Args, Value) {
  dom::Document* document = DocumentOf(ctx);
  std::string out;
  if (document) {
    for (const FaceRuleInfo& r : DocumentFaceRules(document)) {
      for (const std::string* field : {&r.family, &r.src, &r.weight, &r.style, &r.stretch, &r.unicodeRange, &r.variant, &r.featureSettings, &r.variationSettings, &r.display, &r.ascentOverride, &r.descentOverride, &r.lineGapOverride}) {
        out += *field;
        out += '\x1f';
      }
      out += '\x1e';
    }
  }
  return qe::FromWtf8(ctx, out);
}

Value LoadRule(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = DocumentOf(ctx);
  if (!document || args.empty()) return qe::FromBool(false);
  return qe::FromBool(LoadDocumentFace(document, static_cast<size_t>(qe::ToNumber(ctx, args[0]))));
}

Value RuleState(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = DocumentOf(ctx);
  if (!document || args.empty()) return qe::FromInt32(0);
  return qe::FromInt32(DocumentFaceState(document, static_cast<size_t>(qe::ToNumber(ctx, args[0]))));
}

// __solarFontsSetScript(records): the loaded fonts of document.fonts that scripts made; a record is
// "family \x1f weight \x1f style \x1f stretch \x1f unicode-range \x1f handle".
Value SetScript(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = DocumentOf(ctx);
  if (!document) return qe::Undefined();
  const std::string text = StringArg(ctx, args, 0);
  std::vector<ScriptFace> faces;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\x1e', at);
    if (end == std::string::npos) end = text.size();
    std::vector<std::string> f;
    size_t start = at;
    while (start <= end) {
      size_t next = text.find('\x1f', start);
      if (next == std::string::npos || next > end) next = end;
      f.push_back(text.substr(start, next - start));
      start = next + 1;
    }
    if (f.size() >= 6) {
      const int handle = std::atoi(f[5].c_str());
      if (handle >= 0 && static_cast<size_t>(handle) < Handles().size()) faces.push_back({f[0], f[1], f[2], f[3], f[4], Handles()[handle]});
    }
    at = end + 1;
  }
  SetScriptFaces(document, std::move(faces));
  return qe::Undefined();
}

}  // namespace

void InstallFontNatives(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarFontOpen", FontOpen, 1);
  qe::DefineGlobalFunction(ctx, "__solarFontFetch", FontFetch, 1);
  qe::DefineGlobalFunction(ctx, "__solarFontLocal", FontLocal, 1);
  qe::DefineGlobalFunction(ctx, "__solarFontDescriptor", FontDescriptor, 2);
  qe::DefineGlobalFunction(ctx, "__solarFontShorthand", FontShorthand, 1);
  qe::DefineGlobalFunction(ctx, "__solarDocumentFontFaces", DocumentFaces, 0);
  qe::DefineGlobalFunction(ctx, "__solarFontLoadRule", LoadRule, 1);
  qe::DefineGlobalFunction(ctx, "__solarFontRuleState", RuleState, 1);
  qe::DefineGlobalFunction(ctx, "__solarFontsSetScript", SetScript, 1);
}

}  // namespace solar::css
