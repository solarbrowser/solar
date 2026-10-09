#include <algorithm>
#include <cctype>

#include "solar/css/Cssom.h"
#include "solar/css/Style.h"
#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/web/DomBindingsInternal.h"

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

// ---- Prototypes by kind ----

namespace {

enum Proto {
  kSheet, kDeclarations, kMediaList, kRuleList, kStyleSheetList, kRuleBase, kStyleRule, kImportRule, kMediaRule, kFontFaceRule, kPageRule,
  kKeyframesRule, kKeyframeRule, kNamespaceRule, kCounterStyleRule, kSupportsRule, kLayerBlockRule, kLayerStatementRule, kPropertyRule,
  kContainerRule, kScopeRule, kStartingStyleRule, kNestedDeclarationsRule, kProtoCount
};

char g_keys[kProtoCount];

Object* PrototypeOf(Context& ctx, Proto which) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_keys[which])); }

Proto ProtoOfKind(RuleKind kind) {
  switch (kind) {
    case RuleKind::Style: return kStyleRule;
    case RuleKind::Import: return kImportRule;
    case RuleKind::Media: return kMediaRule;
    case RuleKind::FontFace: return kFontFaceRule;
    case RuleKind::Page: return kPageRule;
    case RuleKind::Keyframes: return kKeyframesRule;
    case RuleKind::Keyframe: return kKeyframeRule;
    case RuleKind::Namespace: return kNamespaceRule;
    case RuleKind::CounterStyle: return kCounterStyleRule;
    case RuleKind::Supports: return kSupportsRule;
    case RuleKind::LayerBlock: return kLayerBlockRule;
    case RuleKind::LayerStatement: return kLayerStatementRule;
    case RuleKind::Property: return kPropertyRule;
    case RuleKind::Container: return kContainerRule;
    case RuleKind::Scope: return kScopeRule;
    case RuleKind::StartingStyle: return kStartingStyleRule;
    case RuleKind::NestedDeclarations: return kNestedDeclarationsRule;
  }
  return kRuleBase;
}

template <typename T>
T* Make(Context& ctx, Proto which) {
  T* object = Heap::Allocate<T>();
  object->initialize_prototype(PrototypeOf(ctx, which));
  return object;
}

}  // namespace

CssStyleSheet* NewStyleSheet(Context& ctx) {
  CssStyleSheet* sheet = Make<CssStyleSheet>(ctx, kSheet);
  sheet->media = NewMediaList(ctx);
  sheet->media->ownerSheet = sheet;
  sheet->NoteWrite();
  return sheet;
}
CssRule* NewRule(Context& ctx, RuleKind kind) {
  CssRule* rule = Make<CssRule>(ctx, ProtoOfKind(kind));
  rule->kind = kind;
  return rule;
}
CssDeclarations* NewDeclarations(Context& ctx) { return Make<CssDeclarations>(ctx, kDeclarations); }
MediaList* NewMediaList(Context& ctx) { return Make<MediaList>(ctx, kMediaList); }
CssRuleList* NewRuleList(Context& ctx) { return Make<CssRuleList>(ctx, kRuleList); }

// ---- Declarations: the legacy object hooks ----

std::string PropertyForIdlName(const std::string& name) {
  if (name.empty()) return "";
  if (name == "cssFloat") return "float";
  std::string dashed;
  if (name.find('-') != std::string::npos) {
    if (name.starts_with("--")) return "";
    dashed = name;
    for (char c : dashed) {
      if (c >= 'A' && c <= 'Z') return "";
    }
  } else {
    for (size_t i = 0; i < name.size(); ++i) {
      const char c = name[i];
      if (c >= 'A' && c <= 'Z') {
        dashed += '-';
        dashed += static_cast<char>(c + 0x20);
      } else {
        dashed += c;
      }
    }
    // webkitFoo is -webkit-foo.
    if (dashed.starts_with("webkit-")) dashed = "-" + dashed;
  }
  std::string normalized = dashed;
  if (!NormalizePropertyName(normalized) || normalized != dashed) return "";
  return normalized;
}

namespace {
bool InDocument(dom::Element* element) {
  dom::Node* root = dom::ShadowIncludingRoot(element);
  return root && root->IsDocument();
}
}  // namespace

uint32_t CssDeclarations::IndexedLength(Context&, CssDeclarations& self) {
  if (self.computedElement) return InDocument(self.computedElement) ? static_cast<uint32_t>(ComputedPropertyNames().size()) : 0;
  return static_cast<uint32_t>(self.items.size());
}

bool CssDeclarations::IndexedGetter(Context& ctx, CssDeclarations& self, uint32_t index, Value& out) {
  if (self.computedElement) {
    const std::vector<std::string>& names = ComputedPropertyNames();
    if (index >= names.size() || !InDocument(self.computedElement)) return false;
    out = qe::FromWtf8(ctx, names[index]);
    return true;
  }
  if (index >= self.items.size()) return false;
  out = qe::FromWtf8(ctx, self.items[index].name);
  return true;
}

bool CssDeclarations::NamedGetter(Context& ctx, CssDeclarations& self, const std::string& name, Value& out) {
  const std::string property = PropertyForIdlName(name);
  if (property.empty()) return false;
  out = qe::FromWtf8(ctx, self.ValueOf(property));
  return true;
}

void CssDeclarations::NamedSetter(Context& ctx, CssDeclarations& self, const std::string& name, const Value& value) {
  const std::string property = PropertyForIdlName(name);
  if (property.empty()) return;
  dom::ReactionsScope reactions(ctx);
  const std::string text = qe::ToWtf8(ctx, value);
  if (qe::HasException(ctx)) return;
  if (self.readonly) {
    web::ThrowDomException(ctx, "These styles are computed, and the properties are therefore read-only.", "NoModificationAllowedError");
    return;
  }
  const bool blank = text.find_first_not_of(" \t\n\r\f") == std::string::npos;
  if (blank) self.Remove(ctx, property);
  else self.Set(ctx, property, text, false);
}

namespace {

Value TakeException(Context& ctx) {
  Value exception = ctx.get_exception();
  ctx.clear_exception();
  return exception;
}

template <typename T>
T* This(Context& ctx, const Value& t) {
  T* self = DOMObject::Cast<T>(t);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

std::string Lowercase(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string ArgString(Context& ctx, qe::Args args, size_t i) { return i < args.size() ? qe::ToWtf8(ctx, args[i]) : std::string(); }

Value NullableString(Context& ctx, bool present, const std::string& text) { return present ? qe::FromWtf8(ctx, text) : qe::Null(); }

bool NeedArgs(Context& ctx, qe::Args args, size_t count, const char* what) {
  if (args.size() >= count) return true;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + what + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return false;
}

// ---- CSSStyleDeclaration ----

Value DeclLength(Context& ctx, Value t, qe::Args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  return self ? qe::FromUint32(CssDeclarations::IndexedLength(ctx, *self)) : qe::Undefined();
}

Value DeclGetCssText(Context& ctx, Value t, qe::Args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  return self ? qe::FromWtf8(ctx, self->readonly ? "" : self->Serialize()) : qe::Undefined();
}

bool CheckWritable(Context& ctx, CssDeclarations* self) {
  if (!self->readonly) return true;
  web::ThrowDomException(ctx, "These styles are computed, and the properties are therefore read-only.", "NoModificationAllowedError");
  return false;
}

Value DeclSetCssText(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set cssText")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx) || !CheckWritable(ctx, self)) return qe::Undefined();
  self->SetText(ctx, text);
  return qe::Undefined();
}

Value DeclItem(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "item")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  Value name;
  if (!CssDeclarations::IndexedGetter(ctx, *self, index, name)) return qe::FromWtf8(ctx, "");
  return name;
}

std::string PropertyKey(const std::string& name) { return name.starts_with("--") ? name : Lowercase(name); }

Value DeclGetPropertyValue(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "getPropertyValue")) return qe::Undefined();
  const std::string name = PropertyKey(ArgString(ctx, args, 0));
  return qe::FromWtf8(ctx, self->ValueOf(name));
}

Value DeclGetPropertyPriority(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "getPropertyPriority")) return qe::Undefined();
  return qe::FromWtf8(ctx, self->PriorityOf(PropertyKey(ArgString(ctx, args, 0))));
}

Value DeclSetProperty(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 2, "setProperty")) return qe::Undefined();
  const std::string name = ArgString(ctx, args, 0);
  const std::string value = ArgString(ctx, args, 1);
  const std::string priority = args.size() > 2 && !args[2].is_undefined() ? qe::ToWtf8(ctx, args[2]) : "";
  if (qe::HasException(ctx) || !CheckWritable(ctx, self)) return qe::Undefined();
  const std::string key = PropertyKey(name);
  if (value.find_first_not_of(" \t\n\r\f") == std::string::npos) {
    self->Remove(ctx, key);
    return qe::Undefined();
  }
  if (!priority.empty() && Lowercase(priority) != "important") return qe::Undefined();
  self->Set(ctx, key, value, !priority.empty());
  return qe::Undefined();
}

Value DeclRemoveProperty(Context& ctx, Value t, qe::Args args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "removeProperty")) return qe::Undefined();
  const std::string name = ArgString(ctx, args, 0);
  if (qe::HasException(ctx) || !CheckWritable(ctx, self)) return qe::Undefined();
  return qe::FromWtf8(ctx, self->Remove(ctx, PropertyKey(name)));
}

Value DeclParentRule(Context& ctx, Value t, qe::Args, Value) {
  CssDeclarations* self = This<CssDeclarations>(ctx, t);
  if (!self) return qe::Undefined();
  return self->parentRule ? qe::FromObject(self->parentRule) : qe::Null();
}

// ---- MediaList ----

Value MediaGetText(Context& ctx, Value t, qe::Args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  return self ? qe::FromWtf8(ctx, self->Text()) : qe::Undefined();
}

Value MediaSetText(Context& ctx, Value t, qe::Args args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set mediaText")) return qe::Undefined();
  // [LegacyNullToEmptyString]
  const std::string text = args[0].is_null() ? "" : qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->SetText(text);
  return qe::Undefined();
}

Value MediaLength(Context& ctx, Value t, qe::Args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->queries.size())) : qe::Undefined();
}

Value MediaItem(Context& ctx, Value t, qe::Args args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "item")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  return index < self->queries.size() ? qe::FromWtf8(ctx, self->queries[index]) : qe::Null();
}

Value MediaAppend(Context& ctx, Value t, qe::Args args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "appendMedium")) return qe::Undefined();
  const std::string text = ArgString(ctx, args, 0);
  if (qe::HasException(ctx)) return qe::Undefined();
  MediaList probe;
  (void)probe;
  const ComponentValues parsed = ParseComponentValues(text);
  (void)parsed;
  // Serialized as a one-query list; one that is already there is not added again.
  MediaList* scratch = NewMediaList(ctx);
  scratch->SetText(text);
  if (scratch->queries.size() != 1) return qe::Undefined();
  const std::string& query = scratch->queries[0];
  for (const std::string& existing : self->queries) {
    if (existing == query) return qe::Undefined();
  }
  self->queries.push_back(query);
  return qe::Undefined();
}

Value MediaDelete(Context& ctx, Value t, qe::Args args, Value) {
  MediaList* self = This<MediaList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "deleteMedium")) return qe::Undefined();
  const std::string text = ArgString(ctx, args, 0);
  if (qe::HasException(ctx)) return qe::Undefined();
  MediaList* scratch = NewMediaList(ctx);
  scratch->SetText(text);
  if (scratch->queries.size() == 1) {
    auto it = std::find(self->queries.begin(), self->queries.end(), scratch->queries[0]);
    if (it != self->queries.end()) {
      self->queries.erase(it);
      return qe::Undefined();
    }
  }
  web::ThrowDomException(ctx, "Failed to execute 'deleteMedium' on 'MediaList': The medium provided ('" + text + "') was not found in the list.", "NotFoundError");
  return qe::Undefined();
}

// ---- Rule lists ----

CssRuleList* RuleListOf(Context& ctx, CssStyleSheet* sheet, CssRule* rule) {
  CssRuleList*& cached = sheet ? sheet->ruleList : rule->ruleList;
  if (!cached) {
    cached = NewRuleList(ctx);
    cached->ownerSheet = sheet;
    cached->ownerRule = rule;
    (sheet ? static_cast<DOMObject*>(sheet) : static_cast<DOMObject*>(rule))->NoteWrite();
  }
  return cached;
}

Value RuleListLength(Context& ctx, Value t, qe::Args, Value) {
  CssRuleList* self = This<CssRuleList>(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->Rules().size())) : qe::Undefined();
}

Value RuleListItem(Context& ctx, Value t, qe::Args args, Value) {
  CssRuleList* self = This<CssRuleList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "item")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  const std::vector<CssRule*>& rules = self->Rules();
  return index < rules.size() ? qe::FromObject(rules[index]) : qe::Null();
}

}  // namespace


bool MediaList::IndexedGetter(Context& ctx, MediaList& self, uint32_t index, Value& out) {
  if (index >= self.queries.size()) return false;
  out = qe::FromWtf8(ctx, self.queries[index]);
  return true;
}

bool CssRuleList::IndexedGetter(Context&, CssRuleList& self, uint32_t index, Value& out) {
  const std::vector<CssRule*>& rules = self.Rules();
  if (index >= rules.size()) return false;
  out = qe::FromObject(rules[index]);
  return true;
}

StyleSheetList* NewStyleSheetList(Context& ctx, dom::Document* document) {
  StyleSheetList* list = Make<StyleSheetList>(ctx, kStyleSheetList);
  list->document = document;
  return list;
}

std::vector<CssStyleSheet*> StyleSheetList::Sheets() const {
  std::vector<CssStyleSheet*> sheets;
  if (!document) return sheets;
  for (dom::Node* node = document; node; node = node->NextInTree(document)) {
    dom::Element* element = dom::AsElement(node);
    if (element && element->styleSheet) sheets.push_back(static_cast<CssStyleSheet*>(element->styleSheet));
  }
  return sheets;
}

bool StyleSheetList::IndexedGetter(Context&, StyleSheetList& self, uint32_t index, Value& out) {
  const std::vector<CssStyleSheet*> sheets = self.Sheets();
  if (index >= sheets.size()) return false;
  out = qe::FromObject(sheets[index]);
  return true;
}

namespace {

Value ListItem(Context& ctx, Value t, qe::Args args, Value) {
  StyleSheetList* self = This<StyleSheetList>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "item")) return qe::Undefined();
  const std::vector<CssStyleSheet*> sheets = self->Sheets();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  return index < sheets.size() ? qe::FromObject(sheets[index]) : qe::Null();
}

Value ListLength(Context& ctx, Value t, qe::Args, Value) {
  StyleSheetList* self = This<StyleSheetList>(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->Sheets().size())) : qe::Undefined();
}

// ---- CSSRule ----

Value RuleType(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  // The legacy numbers; the newer kinds have none and answer 0.
  const int kind = static_cast<int>(self->kind);
  return qe::FromUint32(kind < 100 ? static_cast<uint32_t>(kind) : 0);
}

Value RuleCssText(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  return self ? qe::FromWtf8(ctx, self->CssText()) : qe::Undefined();
}

Value RuleSetCssText(Context&, Value, qe::Args, Value) { return qe::Undefined(); }

Value RuleParentRule(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  return self->parentRule ? qe::FromObject(self->parentRule) : qe::Null();
}

Value RuleParentSheet(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  return self->parentSheet ? qe::FromObject(self->parentSheet) : qe::Null();
}

CssRule* ThisRuleOf(Context& ctx, const Value& t, RuleKind kind) {
  CssRule* self = This<CssRule>(ctx, t);
  if (self && self->kind != kind) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

// A grouping rule's cssRules, insertRule and deleteRule.
CssRule* ThisGroup(Context& ctx, const Value& t) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return nullptr;
  switch (self->kind) {
    case RuleKind::Style:
    case RuleKind::Media:
    case RuleKind::Supports:
    case RuleKind::LayerBlock:
    case RuleKind::Container:
    case RuleKind::Scope:
    case RuleKind::StartingStyle:
    case RuleKind::Keyframes:
      return self;
    default:
      qe::ThrowTypeError(ctx, "Illegal invocation");
      return nullptr;
  }
}

Value GroupCssRules(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisGroup(ctx, t);
  return self ? qe::FromObject(RuleListOf(ctx, nullptr, self)) : qe::Undefined();
}

Value GroupInsertRule(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisGroup(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "insertRule")) return qe::Undefined();
  dom::ReactionsScope reactions(ctx);
  const std::string text = ArgString(ctx, args, 0);
  const uint32_t index = args.size() > 1 && !args[1].is_undefined() ? qe::ToUint32(ctx, args[1]) : 0;
  if (qe::HasException(ctx)) return qe::Undefined();
  uint32_t result = 0;
  const std::string error = InsertRule(ctx, self->parentSheet, self, text, index, result);
  if (!error.empty()) {
    web::ThrowDomException(ctx, "Failed to execute 'insertRule' on 'CSSRule': " + error, error);
    return qe::Undefined();
  }
  return qe::FromUint32(result);
}

Value GroupDeleteRule(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisGroup(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "deleteRule")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::string error = DeleteRule(ctx, self->parentSheet, self, index);
  if (!error.empty()) web::ThrowDomException(ctx, "Failed to execute 'deleteRule' on 'CSSRule': " + error, error);
  return qe::Undefined();
}

// CSSStyleRule
Value StyleRuleSelectorText(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Style);
  return self ? qe::FromWtf8(ctx, self->selectorText) : qe::Undefined();
}

Value StyleRuleSetSelectorText(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Style);
  if (!self || !NeedArgs(ctx, args, 1, "set selectorText")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::shared_ptr<SelectorList> nesting;
  for (const CssRule* up = self->parentRule; up; up = up->parentRule) {
    if (up->kind == RuleKind::Style) {
      nesting = up->selectors;
      break;
    }
  }
  const std::string selectorSource = Serialize(Trimmed(ParseComponentValues(text)));
  std::optional<SelectorList> list = nesting ? ParseNestedSelectorList(selectorSource, nesting) : ParseSelectorListForRule(selectorSource);
  if (!list || !ResolveNamespaces(*list, self->parentSheet)) return qe::Undefined();
  // In place: the rules nested in this one hold the very list that & stands for.
  if (self->selectors) *self->selectors = std::move(*list);
  else self->selectors = std::make_shared<SelectorList>(std::move(*list));
  self->selectorText = SerializeSelectorList(*self->selectors);
  NoteStyleChange();
  return qe::Undefined();
}

Value RuleStyle(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  return self->style ? qe::FromObject(self->style) : qe::Null();
}

Value RuleSetStyle(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self || !self->style || !NeedArgs(ctx, args, 1, "set style")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->style->SetText(ctx, text);
  return qe::Undefined();
}

// CSSMediaRule, CSSImportRule
Value RuleMedia(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  return self->media ? qe::FromObject(self->media) : qe::Null();
}

Value RuleSetMedia(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self || !self->media || !NeedArgs(ctx, args, 1, "set media")) return qe::Undefined();
  const std::string text = args[0].is_null() ? "" : qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->media->SetText(text);
  return qe::Undefined();
}

Value RuleConditionText(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self) return qe::Undefined();
  switch (self->kind) {
    case RuleKind::Media: return qe::FromWtf8(ctx, self->media ? self->media->Text() : "");
    case RuleKind::Supports: return qe::FromWtf8(ctx, self->supportsText);
    case RuleKind::Container: return qe::FromWtf8(ctx, self->prelude);
    default: return qe::Undefined();
  }
}

Value RuleHref(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Import);
  return self ? qe::FromWtf8(ctx, self->href) : qe::Undefined();
}

Value RuleImportedSheet(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Import);
  if (!self) return qe::Undefined();
  return self->importedSheet ? qe::FromObject(self->importedSheet) : qe::Null();
}

Value RuleLayerName(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Import);
  if (!self) return qe::Undefined();
  if (self->layerName.empty()) return qe::Null();
  return qe::FromWtf8(ctx, self->layerName == "\x01" ? "" : self->layerName);
}

Value RuleSupportsText(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Import);
  if (!self) return qe::Undefined();
  return self->supportsText.empty() ? qe::Null() : qe::FromWtf8(ctx, self->supportsText);
}

Value RuleNamespaceUri(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Namespace);
  return self ? qe::FromWtf8(ctx, self->namespaceUri) : qe::Undefined();
}

Value RulePrefix(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Namespace);
  return self ? qe::FromWtf8(ctx, self->prefix) : qe::Undefined();
}

Value RuleName(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  return self ? qe::FromWtf8(ctx, self->name) : qe::Undefined();
}

Value RuleSetName(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = This<CssRule>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set name")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->name = text;
  return qe::Undefined();
}

Value RuleSelectorTextPage(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Page);
  return self ? qe::FromWtf8(ctx, self->selectorText) : qe::Undefined();
}

Value RuleKeyText(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframe);
  return self ? qe::FromWtf8(ctx, self->name) : qe::Undefined();
}

Value RuleSetKeyText(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframe);
  if (!self || !NeedArgs(ctx, args, 1, "set keyText")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::optional<std::string> key = NormalizeKeyText(text);
  if (!key) {
    web::ThrowDomException(ctx, "Failed to set 'keyText' on 'CSSKeyframeRule': the keyframe selector is invalid.", "SyntaxError");
    return qe::Undefined();
  }
  self->name = *key;
  NoteStyleChange();
  return qe::Undefined();
}

Value KeyframesLength(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframes);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->rules.size())) : qe::Undefined();
}

// CSSKeyframesRule
Value KeyframesAppend(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframes);
  if (!self || !NeedArgs(ctx, args, 1, "appendRule")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Rule syntax;
  CssRule* frame = ParseRule(text, syntax) ? BuildKeyframe(ctx, syntax, self->parentSheet, self) : nullptr;
  if (!frame) return qe::Undefined();
  self->rules.push_back(frame);
  self->NoteWrite();
  NoteStyleChange();
  return qe::Undefined();
}

int FindKeyframe(CssRule* self, const std::string& key) {
  const std::string wanted = Lowercase(key);
  for (int i = static_cast<int>(self->rules.size()) - 1; i >= 0; --i) {
    if (Lowercase(self->rules[i]->name) == wanted) return i;
  }
  return -1;
}

std::string NormalizeKey(const std::string& key) { return NormalizeKeyText(key).value_or(key); }

Value KeyframesFind(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframes);
  if (!self || !NeedArgs(ctx, args, 1, "findRule")) return qe::Undefined();
  const int index = FindKeyframe(self, NormalizeKey(qe::ToWtf8(ctx, args[0])));
  return index >= 0 ? qe::FromObject(self->rules[index]) : qe::Null();
}

Value KeyframesDelete(Context& ctx, Value t, qe::Args args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Keyframes);
  if (!self || !NeedArgs(ctx, args, 1, "deleteRule")) return qe::Undefined();
  const int index = FindKeyframe(self, NormalizeKey(qe::ToWtf8(ctx, args[0])));
  if (index >= 0) self->rules.erase(self->rules.begin() + index);
  return qe::Undefined();
}

// CSSLayerStatementRule.nameList, CSSContainerRule, CSSScopeRule, CSSPropertyRule
Value LayerNameList(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::LayerStatement);
  if (!self) return qe::Undefined();
  Value array = qe::NewArray(ctx);
  size_t start = 0;
  const std::string& names = self->layerName;
  while (start <= names.size()) {
    size_t comma = names.find(", ", start);
    const std::string one = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!one.empty()) qe::ArrayPush(ctx, array, qe::FromWtf8(ctx, one));
    if (comma == std::string::npos) break;
    start = comma + 2;
  }
  return array;
}

Value ContainerName(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Container);
  if (!self) return qe::Undefined();
  const ComponentValues values = Trimmed(ParseComponentValues(self->prelude));
  if (!values.empty() && values[0].IsIdent() && values[0].token.value != "not") return qe::FromWtf8(ctx, values[0].token.value);
  return qe::FromWtf8(ctx, "");
}

Value ContainerQuery(Context& ctx, Value t, qe::Args, Value) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Container);
  if (!self) return qe::Undefined();
  const ComponentValues values = Trimmed(ParseComponentValues(self->prelude));
  size_t skip = !values.empty() && values[0].IsIdent() && values[0].token.value != "not" ? 1 : 0;
  while (skip < values.size() && values[skip].IsWhitespace()) ++skip;
  return qe::FromWtf8(ctx, Serialize(ComponentValues(values.begin() + skip, values.end())));
}

// The (selector) blocks of a scope rule's prelude: before `to`, and after it.
Value ScopePart(Context& ctx, Value t, bool end) {
  CssRule* self = ThisRuleOf(ctx, t, RuleKind::Scope);
  if (!self) return qe::Undefined();
  const ComponentValues values = Trimmed(ParseComponentValues(self->prelude));
  bool afterTo = false;
  for (const ComponentValue& v : values) {
    if (v.IsIdent() && Lowercase(v.token.value) == "to") {
      afterTo = true;
      continue;
    }
    if (v.IsBlock(Token::Type::LeftParen) && afterTo == end) return qe::FromWtf8(ctx, Serialize(Trimmed(v.children)));
  }
  return qe::Null();
}

Value ScopeStart(Context& ctx, Value t, qe::Args, Value) { return ScopePart(ctx, t, false); }
Value ScopeEnd(Context& ctx, Value t, qe::Args, Value) { return ScopePart(ctx, t, true); }

}  // namespace

// ---- Style sheets ----

namespace {

Value SheetType(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  return self ? qe::FromWtf8(ctx, self->type) : qe::Undefined();
}

Value SheetHref(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  return self ? NullableString(ctx, self->hasHref, self->href) : qe::Undefined();
}

Value SheetOwnerNode(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self) return qe::Undefined();
  return self->ownerNode ? qe::FromObject(self->ownerNode) : qe::Null();
}

Value SheetParent(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self) return qe::Undefined();
  return self->parentSheet ? qe::FromObject(self->parentSheet) : qe::Null();
}

Value SheetOwnerRule(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self) return qe::Undefined();
  return self->ownerRule ? qe::FromObject(self->ownerRule) : qe::Null();
}

Value SheetTitle(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  return self ? NullableString(ctx, self->hasTitle, self->title) : qe::Undefined();
}

Value SheetMedia(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  return self ? qe::FromObject(self->media) : qe::Undefined();
}

Value SheetSetMedia(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set media")) return qe::Undefined();
  const std::string text = args[0].is_null() ? "" : qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->media->SetText(text);
  return qe::Undefined();
}

Value SheetDisabled(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  return self ? qe::FromBool(self->disabled) : qe::Undefined();
}

Value SheetSetDisabled(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set disabled")) return qe::Undefined();
  self->disabled = qe::ToBoolean(args[0]);
  NoteStyleChange();
  return qe::Undefined();
}

bool CheckReadable(Context& ctx, CssStyleSheet* self) {
  if (self->originClean) return true;
  web::ThrowDomException(ctx, "Cannot access rules", "SecurityError");
  return false;
}

Value SheetCssRules(Context& ctx, Value t, qe::Args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !CheckReadable(ctx, self)) return qe::Undefined();
  return qe::FromObject(RuleListOf(ctx, self, nullptr));
}

Value SheetInsertRule(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "insertRule") || !CheckReadable(ctx, self)) return qe::Undefined();
  dom::ReactionsScope reactions(ctx);
  const std::string text = ArgString(ctx, args, 0);
  const uint32_t index = args.size() > 1 && !args[1].is_undefined() ? qe::ToUint32(ctx, args[1]) : 0;
  if (qe::HasException(ctx)) return qe::Undefined();
  uint32_t result = 0;
  const std::string error = InsertRule(ctx, self, nullptr, text, index, result);
  if (!error.empty()) {
    web::ThrowDomException(ctx, "Failed to execute 'insertRule' on 'CSSStyleSheet': " + error, error);
    return qe::Undefined();
  }
  return qe::FromUint32(result);
}

Value SheetDeleteRule(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "deleteRule") || !CheckReadable(ctx, self)) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::string error = DeleteRule(ctx, self, nullptr, index);
  if (!error.empty()) web::ThrowDomException(ctx, "Failed to execute 'deleteRule' on 'CSSStyleSheet': " + error, error);
  return qe::Undefined();
}

// The legacy addRule(selector, block, index) and removeRule(index).
Value SheetAddRule(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "addRule") || !CheckReadable(ctx, self)) return qe::Undefined();
  const std::string selector = ArgString(ctx, args, 0);
  const std::string block = args.size() > 1 && !args[1].is_undefined() ? qe::ToWtf8(ctx, args[1]) : "";
  const uint32_t index = args.size() > 2 && !args[2].is_undefined() ? qe::ToUint32(ctx, args[2]) : static_cast<uint32_t>(self->rules.size());
  if (qe::HasException(ctx)) return qe::Undefined();
  uint32_t result = 0;
  const std::string error = InsertRule(ctx, self, nullptr, selector + " { " + block + " }", index, result);
  if (!error.empty()) {
    web::ThrowDomException(ctx, "Failed to execute 'addRule' on 'CSSStyleSheet': " + error, error);
    return qe::Undefined();
  }
  return qe::FromInt32(-1);
}

Value SheetRemoveRule(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !CheckReadable(ctx, self)) return qe::Undefined();
  const uint32_t index = args.empty() || args[0].is_undefined() ? 0 : qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::string error = DeleteRule(ctx, self, nullptr, index);
  if (!error.empty()) web::ThrowDomException(ctx, "Failed to execute 'removeRule' on 'CSSStyleSheet': " + error, error);
  return qe::Undefined();
}

Value SheetReplaceSync(Context& ctx, Value t, qe::Args args, Value) {
  CssStyleSheet* self = This<CssStyleSheet>(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "replaceSync")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!self->constructed) {
    web::ThrowDomException(ctx, "Can't call replaceSync on non-constructed CSSStyleSheets.", "NotAllowedError");
    return qe::Undefined();
  }
  ParseSheetInto(ctx, self, text);
  return qe::Undefined();
}

Value SheetReplace(Context& ctx, Value t, qe::Args args, Value) {
  qe::PromiseCapability promise = qe::NewPromiseCapability(ctx);
  CssStyleSheet* self = DOMObject::Cast<CssStyleSheet>(t);
  Value outcome;
  bool rejected = false;
  if (!self) {
    outcome = web::NewDomException(ctx, "Illegal invocation", "TypeError");
    rejected = true;
  } else if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'replace' on 'CSSStyleSheet': 1 argument required, but only 0 present.");
    outcome = TakeException(ctx);
    rejected = true;
  } else if (!self->constructed) {
    outcome = web::NewDomException(ctx, "Can't call replace on non-constructed CSSStyleSheets.", "NotAllowedError");
    rejected = true;
  } else {
    const std::string text = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) {
      outcome = TakeException(ctx);
      rejected = true;
    } else {
      ParseSheetInto(ctx, self, text);
      outcome = qe::FromObject(self);
    }
  }
  qe::Call(ctx, rejected ? promise.reject : promise.resolve, qe::Undefined(), qe::Args(&outcome, 1));
  return promise.promise;
}

Value ConstructSheet(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (newTarget.is_undefined()) {
    qe::ThrowTypeError(ctx, "Failed to construct 'CSSStyleSheet': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget, &g_keys[kSheet]);
  if (qe::HasException(ctx)) return qe::Undefined();
  CssStyleSheet* sheet = NewStyleSheet(ctx);
  if (prototype) sheet->initialize_prototype(prototype);
  sheet->constructed = true;
  sheet->disallowImport = true;
  sheet->constructorDocument = dom::AssociatedDocument(ctx);
  sheet->baseUrl = sheet->constructorDocument ? sheet->constructorDocument->url : "";
  if (!args.empty() && !args[0].is_undefined() && !args[0].is_null()) {
    if (!qe::IsObject(args[0])) {
      qe::ThrowTypeError(ctx, "Failed to construct 'CSSStyleSheet': The provided value is not of type 'CSSStyleSheetInit'.");
      return qe::Undefined();
    }
    Value base = qe::Get(ctx, args[0], "baseURL");
    if (!base.is_undefined()) sheet->baseUrl = qe::ToWtf8(ctx, base);
    Value media = qe::Get(ctx, args[0], "media");
    if (!media.is_undefined()) {
      if (qe::IsObject(media)) {
        if (MediaList* list = DOMObject::Cast<MediaList>(media)) sheet->media->queries = list->queries;
      } else {
        sheet->media->SetText(qe::ToWtf8(ctx, media));
      }
    }
    sheet->disabled = qe::ToBoolean(qe::Get(ctx, args[0], "disabled"));
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::FromObject(sheet);
}

}  // namespace

// ---- Style and link elements ----

void UpdateStyleElement(Context& ctx, dom::Element* element) {
  NoteStyleChange();
  const bool wasLinked = element->styleSheet != nullptr;
  element->styleSheet = nullptr;
  if (!element->IsHtml("style")) return;
  dom::Node* root = dom::ShadowIncludingRoot(element);
  if (!root || !root->IsDocument()) {
    if (wasLinked) element->NoteWrite();
    return;
  }
  if (const dom::Attr* type = element->FindAttribute("", "type")) {
    if (!type->value.empty() && Lowercase(type->value) != "text/css") return;
  }
  CssStyleSheet* sheet = NewStyleSheet(ctx);
  sheet->ownerNode = element;
  sheet->baseUrl = static_cast<dom::Document*>(root)->url;
  if (const dom::Attr* media = element->FindAttribute("", "media")) sheet->media->SetText(media->value);
  if (const dom::Attr* title = element->FindAttribute("", "title")) {
    if (!title->value.empty()) {
      sheet->title = title->value;
      sheet->hasTitle = true;
    }
  }
  ParseSheetInto(ctx, sheet, element->DescendantText());
  element->styleSheet = sheet;
  element->NoteWrite();
}

CssStyleSheet* StyleSheetOfElement(Context&, dom::Element* element) { return static_cast<CssStyleSheet*>(element->styleSheet); }

namespace {

Value ElementSheet(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  return self->styleSheet ? qe::FromObject(self->styleSheet) : qe::Null();
}

Value DocumentStyleSheets(Context& ctx, Value t, qe::Args, Value) {
  dom::Node* node = dom::ThisNode(ctx, t);
  if (!node) return qe::Undefined();
  if (!node->IsDocument()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  dom::Document* document = static_cast<dom::Document*>(node);
  if (!document->styleSheetList) {
    document->styleSheetList = NewStyleSheetList(ctx, document);
    document->NoteWrite();
  }
  return qe::FromObject(document->styleSheetList);
}

// element.style
Value ElementStyle(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->inlineStyle) {
    CssDeclarations* declarations = NewDeclarations(ctx);
    declarations->ownerElement = self;
    declarations->updatingAttribute = true;
    if (const dom::Attr* style = self->FindAttribute("", "style")) declarations->SetText(ctx, style->value);
    declarations->updatingAttribute = false;
    self->inlineStyle = declarations;
    self->NoteWrite();
  }
  return qe::FromObject(self->inlineStyle);
}

Value ElementSetStyle(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self || !NeedArgs(ctx, args, 1, "set style")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value style = ElementStyle(ctx, t, qe::Args(), qe::Undefined());
  if (CssDeclarations* declarations = DOMObject::Cast<CssDeclarations>(style)) declarations->SetText(ctx, text);
  return qe::Undefined();
}

Object* DefineRule(Context& ctx, Proto which, const char* name, Object* parent, Proto parentProto = kProtoCount) {
  (void)parentProto;
  qe::ClassRef cls = qe::DefineClass(ctx, name, IllegalConstructor, 0, parent);
  qe::SetRealmData(ctx, &g_keys[which], cls.prototype);
  qe::DefineGlobal(ctx, name, cls.constructor);
  return cls.prototype;
}

void Constant(Context& ctx, Object* object, const char* name, uint32_t value) {
  qe::Descriptor d;
  d.has_value = true;
  d.value = qe::FromUint32(value);
  d.has_writable = true;
  d.writable = false;
  d.has_enumerable = true;
  d.enumerable = true;
  d.has_configurable = true;
  d.configurable = false;
  qe::DefineProperty(ctx, qe::FromObject(object), name, d);
}

}  // namespace

void StyleAttributeChanged(dom::Element* element) {
  CssDeclarations* declarations = static_cast<CssDeclarations*>(element->inlineStyle);
  if (!declarations || declarations->updatingAttribute) return;
  Context* ctx = element->nodeDocument ? element->nodeDocument->context : nullptr;
  if (!ctx) return;
  const dom::Attr* style = element->FindAttribute("", "style");
  declarations->updatingAttribute = true;
  declarations->SetText(*ctx, style ? style->value : "");
  declarations->updatingAttribute = false;
}

// getComputedStyle(element, pseudo): the computed values of the element, read when they are asked for.
Value GetComputedStyle(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'getComputedStyle' on 'Window': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  dom::Element* element = DOMObject::Cast<dom::Element>(args[0]);
  if (!element) {
    qe::ThrowTypeError(ctx, "Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
    return qe::Undefined();
  }
  CssDeclarations* declarations = NewDeclarations(ctx);
  declarations->readonly = true;
  // The pseudo-element asked for: nothing, one this engine styles, or one it does not (an empty declaration).
  std::string pseudo;
  bool known = true;
  if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
    const std::string text = qe::ToWtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
    known = false;
    if (!text.empty()) {
      const std::optional<SelectorList> list = ParseSelectorList(text);
      if (list && list->size() == 1 && (*list)[0].compounds.size() == 1 && (*list)[0].compounds[0].simples.size() == 1) {
        const SimpleSelector& simple = (*list)[0].compounds[0].simples[0];
        static const char* const styled[] = {"before", "after", "first-line", "first-letter", "marker", "placeholder", "selection", "backdrop"};
        if (simple.kind == SimpleSelector::Kind::PseudoElement && simple.value.empty()) {
          for (const char* name : styled) {
            if (simple.name == name) {
              pseudo = simple.name;
              known = true;
            }
          }
        }
      }
    } else {
      known = true;
    }
  }
  if (known) {
    declarations->computedElement = element;
    declarations->computedContext = element->nodeDocument && element->nodeDocument->context ? element->nodeDocument->context : &ctx;
    declarations->computedPseudo = pseudo;
  }
  return qe::FromObject(declarations);
}

// CSS.supports(property, value) and CSS.supports(condition).
Value CssSupports(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'supports' on 'CSS': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string first = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (args.size() >= 2) {
    const std::string second = qe::ToWtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
    return qe::FromBool(SupportsDeclaration(first, second));
  }
  const ComponentValues condition = ParseComponentValues(first);
  if (SupportsCondition(condition)) return qe::FromBool(true);
  // A declaration without its parentheses is taken as if it had them.
  ComponentValue block;
  block.kind = ComponentValue::Kind::Block;
  block.open = Token::Type::LeftParen;
  block.children = condition;
  return qe::FromBool(SupportsCondition({block}));
}

void InstallCssSupports(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarCssSupports", CssSupports, 1);
}

void DefineCssomClasses(Context& ctx) {
  // CSSStyleDeclaration
  qe::ClassRef declaration = qe::DefineClass(ctx, "CSSStyleDeclaration", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_keys[kDeclarations], declaration.prototype);
  Object* d = declaration.prototype;
  qe::DefineAccessor(d, "length", DeclLength, nullptr);
  qe::DefineAccessor(d, "cssText", DeclGetCssText, dom::Reactions<DeclSetCssText>);
  qe::DefineAccessor(d, "parentRule", DeclParentRule, nullptr);
  qe::DefineMethod(d, "item", DeclItem, 1);
  qe::DefineMethod(d, "getPropertyValue", DeclGetPropertyValue, 1);
  qe::DefineMethod(d, "getPropertyPriority", DeclGetPropertyPriority, 1);
  qe::DefineMethod(d, "setProperty", dom::Reactions<DeclSetProperty>, 2);
  qe::DefineMethod(d, "removeProperty", dom::Reactions<DeclRemoveProperty>, 1);
  qe::DefineGlobal(ctx, "CSSStyleDeclaration", declaration.constructor);
  qe::DefineGlobalFunction(ctx, "getComputedStyle", GetComputedStyle, 1);

  DefineMediaQueryList(ctx);

  // MediaList
  qe::ClassRef media = qe::DefineClass(ctx, "MediaList", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_keys[kMediaList], media.prototype);
  qe::DefineAccessor(media.prototype, "mediaText", MediaGetText, MediaSetText);
  qe::DefineAccessor(media.prototype, "length", MediaLength, nullptr);
  qe::DefineMethod(media.prototype, "item", MediaItem, 1);
  qe::DefineMethod(media.prototype, "appendMedium", MediaAppend, 1);
  qe::DefineMethod(media.prototype, "deleteMedium", MediaDelete, 1);
  qe::DefineGlobal(ctx, "MediaList", media.constructor);

  // CSSRuleList, StyleSheetList
  qe::ClassRef ruleList = qe::DefineClass(ctx, "CSSRuleList", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_keys[kRuleList], ruleList.prototype);
  qe::DefineAccessor(ruleList.prototype, "length", RuleListLength, nullptr);
  qe::DefineMethod(ruleList.prototype, "item", RuleListItem, 1);
  qe::DefineGlobal(ctx, "CSSRuleList", ruleList.constructor);
  qe::ClassRef sheetList = qe::DefineClass(ctx, "StyleSheetList", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_keys[kStyleSheetList], sheetList.prototype);
  qe::DefineAccessor(sheetList.prototype, "length", ListLength, nullptr);
  qe::DefineMethod(sheetList.prototype, "item", ListItem, 1);
  qe::DefineGlobal(ctx, "StyleSheetList", sheetList.constructor);

  // StyleSheet and CSSStyleSheet
  qe::ClassRef base = qe::DefineClass(ctx, "StyleSheet", IllegalConstructor, 0);
  qe::DefineAccessor(base.prototype, "type", SheetType, nullptr);
  qe::DefineAccessor(base.prototype, "href", SheetHref, nullptr);
  qe::DefineAccessor(base.prototype, "ownerNode", SheetOwnerNode, nullptr);
  qe::DefineAccessor(base.prototype, "parentStyleSheet", SheetParent, nullptr);
  qe::DefineAccessor(base.prototype, "title", SheetTitle, nullptr);
  qe::DefineAccessor(base.prototype, "media", SheetMedia, SheetSetMedia);
  qe::DefineAccessor(base.prototype, "disabled", SheetDisabled, SheetSetDisabled);
  qe::DefineGlobal(ctx, "StyleSheet", base.constructor);
  qe::ClassRef sheet = qe::DefineClass(ctx, "CSSStyleSheet", ConstructSheet, 0, base.prototype);
  qe::SetRealmData(ctx, &g_keys[kSheet], sheet.prototype);
  Object* s = sheet.prototype;
  qe::DefineAccessor(s, "ownerRule", SheetOwnerRule, nullptr);
  qe::DefineAccessor(s, "cssRules", SheetCssRules, nullptr);
  qe::DefineAccessor(s, "rules", SheetCssRules, nullptr);
  qe::DefineMethod(s, "insertRule", dom::Reactions<SheetInsertRule>, 1);
  qe::DefineMethod(s, "deleteRule", dom::Reactions<SheetDeleteRule>, 1);
  qe::DefineMethod(s, "addRule", dom::Reactions<SheetAddRule>, 0);
  qe::DefineMethod(s, "removeRule", dom::Reactions<SheetRemoveRule>, 0);
  qe::DefineMethod(s, "replace", SheetReplace, 1);
  qe::DefineMethod(s, "replaceSync", SheetReplaceSync, 1);
  qe::DefineGlobal(ctx, "CSSStyleSheet", sheet.constructor);

  // CSSRule and its kinds
  qe::ClassRef rule = qe::DefineClass(ctx, "CSSRule", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_keys[kRuleBase], rule.prototype);
  qe::DefineAccessor(rule.prototype, "type", RuleType, nullptr);
  qe::DefineAccessor(rule.prototype, "cssText", RuleCssText, RuleSetCssText);
  qe::DefineAccessor(rule.prototype, "parentRule", RuleParentRule, nullptr);
  qe::DefineAccessor(rule.prototype, "parentStyleSheet", RuleParentSheet, nullptr);
  static const std::pair<const char*, uint32_t> kConstants[] = {
      {"STYLE_RULE", 1}, {"CHARSET_RULE", 2}, {"IMPORT_RULE", 3}, {"MEDIA_RULE", 4}, {"FONT_FACE_RULE", 5}, {"PAGE_RULE", 6},
      {"MARGIN_RULE", 9}, {"NAMESPACE_RULE", 10}, {"KEYFRAMES_RULE", 7}, {"KEYFRAME_RULE", 8}, {"COUNTER_STYLE_RULE", 11}, {"SUPPORTS_RULE", 12},
      {"FONT_FEATURE_VALUES_RULE", 14}};
  for (const auto& [name, value] : kConstants) {
    Constant(ctx, rule.constructor, name, value);
    Constant(ctx, rule.prototype, name, value);
  }
  qe::DefineGlobal(ctx, "CSSRule", rule.constructor);
  Object* ruleBase = rule.prototype;

  // CSSGroupingRule: rules that hold rules.
  qe::ClassRef grouping = qe::DefineClass(ctx, "CSSGroupingRule", IllegalConstructor, 0, ruleBase);
  qe::DefineAccessor(grouping.prototype, "cssRules", GroupCssRules, nullptr);
  qe::DefineMethod(grouping.prototype, "insertRule", dom::Reactions<GroupInsertRule>, 1);
  qe::DefineMethod(grouping.prototype, "deleteRule", dom::Reactions<GroupDeleteRule>, 1);
  qe::DefineGlobal(ctx, "CSSGroupingRule", grouping.constructor);
  qe::ClassRef condition = qe::DefineClass(ctx, "CSSConditionRule", IllegalConstructor, 0, grouping.prototype);
  qe::DefineAccessor(condition.prototype, "conditionText", RuleConditionText, nullptr);
  qe::DefineGlobal(ctx, "CSSConditionRule", condition.constructor);

  Object* style = DefineRule(ctx, kStyleRule, "CSSStyleRule", grouping.prototype);
  qe::DefineAccessor(style, "selectorText", StyleRuleSelectorText, StyleRuleSetSelectorText);
  qe::DefineAccessor(style, "style", RuleStyle, RuleSetStyle);
  Object* mediaRule = DefineRule(ctx, kMediaRule, "CSSMediaRule", condition.prototype);
  qe::DefineAccessor(mediaRule, "media", RuleMedia, RuleSetMedia);
  DefineRule(ctx, kSupportsRule, "CSSSupportsRule", condition.prototype);
  Object* container = DefineRule(ctx, kContainerRule, "CSSContainerRule", condition.prototype);
  qe::DefineAccessor(container, "containerName", ContainerName, nullptr);
  qe::DefineAccessor(container, "containerQuery", ContainerQuery, nullptr);
  Object* layerBlock = DefineRule(ctx, kLayerBlockRule, "CSSLayerBlockRule", grouping.prototype);
  qe::DefineAccessor(layerBlock, "name", RuleName, nullptr);
  Object* scope = DefineRule(ctx, kScopeRule, "CSSScopeRule", grouping.prototype);
  qe::DefineAccessor(scope, "start", ScopeStart, nullptr);
  qe::DefineAccessor(scope, "end", ScopeEnd, nullptr);
  DefineRule(ctx, kStartingStyleRule, "CSSStartingStyleRule", grouping.prototype);
  Object* nested = DefineRule(ctx, kNestedDeclarationsRule, "CSSNestedDeclarations", ruleBase);
  qe::DefineAccessor(nested, "style", RuleStyle, RuleSetStyle);

  Object* import = DefineRule(ctx, kImportRule, "CSSImportRule", ruleBase);
  qe::DefineAccessor(import, "href", RuleHref, nullptr);
  qe::DefineAccessor(import, "media", RuleMedia, RuleSetMedia);
  qe::DefineAccessor(import, "styleSheet", RuleImportedSheet, nullptr);
  qe::DefineAccessor(import, "layerName", RuleLayerName, nullptr);
  qe::DefineAccessor(import, "supportsText", RuleSupportsText, nullptr);
  Object* ns = DefineRule(ctx, kNamespaceRule, "CSSNamespaceRule", ruleBase);
  qe::DefineAccessor(ns, "namespaceURI", RuleNamespaceUri, nullptr);
  qe::DefineAccessor(ns, "prefix", RulePrefix, nullptr);
  Object* fontFace = DefineRule(ctx, kFontFaceRule, "CSSFontFaceRule", ruleBase);
  qe::DefineAccessor(fontFace, "style", RuleStyle, nullptr);
  Object* page = DefineRule(ctx, kPageRule, "CSSPageRule", grouping.prototype);
  qe::DefineAccessor(page, "selectorText", RuleSelectorTextPage, nullptr);
  qe::DefineAccessor(page, "style", RuleStyle, RuleSetStyle);
  Object* keyframes = DefineRule(ctx, kKeyframesRule, "CSSKeyframesRule", ruleBase);
  qe::DefineAccessor(keyframes, "name", RuleName, RuleSetName);
  qe::DefineAccessor(keyframes, "cssRules", GroupCssRules, nullptr);
  qe::DefineAccessor(keyframes, "length", KeyframesLength, nullptr);
  qe::DefineMethod(keyframes, "appendRule", KeyframesAppend, 1);
  qe::DefineMethod(keyframes, "deleteRule", KeyframesDelete, 1);
  qe::DefineMethod(keyframes, "findRule", KeyframesFind, 1);
  Object* keyframe = DefineRule(ctx, kKeyframeRule, "CSSKeyframeRule", ruleBase);
  qe::DefineAccessor(keyframe, "keyText", RuleKeyText, RuleSetKeyText);
  qe::DefineAccessor(keyframe, "style", RuleStyle, RuleSetStyle);
  Object* counter = DefineRule(ctx, kCounterStyleRule, "CSSCounterStyleRule", ruleBase);
  qe::DefineAccessor(counter, "name", RuleName, RuleSetName);
  Object* property = DefineRule(ctx, kPropertyRule, "CSSPropertyRule", ruleBase);
  qe::DefineAccessor(property, "name", RuleName, nullptr);
  Object* layerStatement = DefineRule(ctx, kLayerStatementRule, "CSSLayerStatementRule", ruleBase);
  qe::DefineAccessor(layerStatement, "nameList", LayerNameList, nullptr);

  // The interface objects inherit from their parents' as the prototypes do.
  const std::pair<const char*, const char*> inherits[] = {
      {"CSSStyleSheet", "StyleSheet"}, {"CSSGroupingRule", "CSSRule"}, {"CSSConditionRule", "CSSGroupingRule"}, {"CSSStyleRule", "CSSGroupingRule"},
      {"CSSMediaRule", "CSSConditionRule"}, {"CSSSupportsRule", "CSSConditionRule"}, {"CSSContainerRule", "CSSConditionRule"},
      {"CSSLayerBlockRule", "CSSGroupingRule"}, {"CSSScopeRule", "CSSGroupingRule"}, {"CSSStartingStyleRule", "CSSGroupingRule"}, {"CSSPageRule", "CSSGroupingRule"},
      {"CSSNestedDeclarations", "CSSRule"}, {"CSSImportRule", "CSSRule"}, {"CSSNamespaceRule", "CSSRule"}, {"CSSFontFaceRule", "CSSRule"},
      {"CSSKeyframesRule", "CSSRule"}, {"CSSKeyframeRule", "CSSRule"}, {"CSSCounterStyleRule", "CSSRule"}, {"CSSPropertyRule", "CSSRule"},
      {"CSSLayerStatementRule", "CSSRule"}};
  const Value global = qe::FromObject(ctx.get_global_object());
  for (const auto& [child, parent] : inherits) qe::SetPrototypeOf(ctx, qe::Get(ctx, global, child), qe::Get(ctx, global, parent));

  // The members of the DOM that lead to sheets and styles.
  if (Object* element = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement)) qe::DefineAccessor(element, "style", ElementStyle, ElementSetStyle);
  if (Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document)) qe::DefineAccessor(document, "styleSheets", DocumentStyleSheets, nullptr);
  for (const char* tag : {"style", "link"}) {
    Object* prototype = dom::HtmlElementPrototype(ctx, tag);
    if (prototype && prototype != dom::InterfacePrototype(ctx, dom::Interface::HtmlElement)) qe::DefineAccessor(prototype, "sheet", ElementSheet, nullptr);
  }
}

}  // namespace solar::css
