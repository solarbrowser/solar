#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "solar/dom/CustomElements.h"
#include "solar/dom/Mutation.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

struct Interface {
  const char* name;
  std::initializer_list<const char*> tags;
};

// The interfaces the HTML Standard gives its elements. What each adds to HTMLElement is the standard's
// element-by-element work; here they are the objects script can test for and that carry that work.
const Interface kInterfaces[] = {
    {"HTMLAnchorElement", {"a"}},
    {"HTMLAreaElement", {"area"}},
    {"HTMLAudioElement", {"audio"}},
    {"HTMLBaseElement", {"base"}},
    {"HTMLBodyElement", {"body"}},
    {"HTMLBRElement", {"br"}},
    {"HTMLButtonElement", {"button"}},
    {"HTMLCanvasElement", {"canvas"}},
    {"HTMLDataElement", {"data"}},
    {"HTMLDataListElement", {"datalist"}},
    {"HTMLDetailsElement", {"details"}},
    {"HTMLDialogElement", {"dialog"}},
    {"HTMLDirectoryElement", {"dir"}},
    {"HTMLDivElement", {"div"}},
    {"HTMLDListElement", {"dl"}},
    {"HTMLEmbedElement", {"embed"}},
    {"HTMLFieldSetElement", {"fieldset"}},
    {"HTMLFontElement", {"font"}},
    {"HTMLFormElement", {"form"}},
    {"HTMLFrameElement", {"frame"}},
    {"HTMLFrameSetElement", {"frameset"}},
    {"HTMLHeadElement", {"head"}},
    {"HTMLHeadingElement", {"h1", "h2", "h3", "h4", "h5", "h6"}},
    {"HTMLHRElement", {"hr"}},
    {"HTMLHtmlElement", {"html"}},
    {"HTMLIFrameElement", {"iframe"}},
    {"HTMLImageElement", {"img"}},
    {"HTMLInputElement", {"input"}},
    {"HTMLLabelElement", {"label"}},
    {"HTMLLegendElement", {"legend"}},
    {"HTMLLIElement", {"li"}},
    {"HTMLLinkElement", {"link"}},
    {"HTMLMapElement", {"map"}},
    {"HTMLMarqueeElement", {"marquee"}},
    {"HTMLMenuElement", {"menu"}},
    {"HTMLMetaElement", {"meta"}},
    {"HTMLMeterElement", {"meter"}},
    {"HTMLModElement", {"ins", "del"}},
    {"HTMLObjectElement", {"object"}},
    {"HTMLOListElement", {"ol"}},
    {"HTMLOptGroupElement", {"optgroup"}},
    {"HTMLOptionElement", {"option"}},
    {"HTMLOutputElement", {"output"}},
    {"HTMLParagraphElement", {"p"}},
    {"HTMLParamElement", {"param"}},
    {"HTMLPictureElement", {"picture"}},
    {"HTMLPreElement", {"pre", "listing", "xmp"}},
    {"HTMLProgressElement", {"progress"}},
    {"HTMLQuoteElement", {"blockquote", "q"}},
    {"HTMLScriptElement", {"script"}},
    {"HTMLSelectElement", {"select"}},
    {"HTMLSlotElement", {"slot"}},
    {"HTMLSourceElement", {"source"}},
    {"HTMLSpanElement", {"span"}},
    {"HTMLStyleElement", {"style"}},
    {"HTMLTableCaptionElement", {"caption"}},
    {"HTMLTableCellElement", {"td", "th"}},
    {"HTMLTableColElement", {"col", "colgroup"}},
    {"HTMLTableElement", {"table"}},
    {"HTMLTableRowElement", {"tr"}},
    {"HTMLTableSectionElement", {"thead", "tbody", "tfoot"}},
    {"HTMLTemplateElement", {"template"}},
    {"HTMLTextAreaElement", {"textarea"}},
    {"HTMLTimeElement", {"time"}},
    {"HTMLTitleElement", {"title"}},
    {"HTMLTrackElement", {"track"}},
    {"HTMLUListElement", {"ul"}},
    {"HTMLVideoElement", {"video"}},
};

// The elements that have no interface of their own: HTMLElement is theirs.
const char* const kPlainTags[] = {"abbr", "acronym", "address", "article", "aside", "b", "basefont", "bdi", "bdo", "bgsound", "big", "center", "cite", "code", "dd", "dfn", "dt",
                                  "em", "figcaption", "figure", "footer", "header", "hgroup", "i", "kbd", "keygen", "main", "mark", "nav", "nobr", "noembed", "noframes",
                                  "noscript", "rb", "rp", "rt", "rtc", "ruby", "s", "samp", "search", "section", "small", "strike", "strong", "sub", "summary", "sup", "tt",
                                  "u", "var", "wbr"};

Value GetTemplateContent(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->IsHtml("template") || !self->templateContents) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return qe::FromObject(self->templateContents);
}

// ---- HTMLSlotElement ----

dom::Element* ThisSlot(Context& ctx, const Value& t) {
  dom::Element* slot = dom::ThisElement(ctx, t);
  if (slot && !slot->IsHtml("slot")) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return slot;
}

Value GetSlotName(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisSlot(ctx, t);
  return self ? qe::FromWtf8(ctx, dom::SlotName(self)) : qe::Undefined();
}

Value SetSlotName(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisSlot(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, "name", std::move(text));
  return qe::Undefined();
}

// The shadowroot* attributes of a template: shadowRootMode and shadowRootSlotAssignment are enumerations
// that reflect only their known values, the rest are booleans.
dom::Element* ThisTemplate(Context& ctx, const Value& t) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (self && !self->IsHtml("template")) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

template <const char* Attribute, const char* First, const char* Second, const char* Default>
Value GetEnumerated(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisTemplate(ctx, t);
  if (!self) return qe::Undefined();
  std::string value = dom::GetAttribute(self, Attribute).value_or("");
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  if (value == First || value == Second) return qe::FromWtf8(ctx, value);
  return qe::FromWtf8(ctx, Default);
}

template <const char* Attribute>
Value SetString(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisTemplate(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, Attribute, std::move(text));
  return qe::Undefined();
}

template <const char* Attribute>
Value GetBoolean(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisTemplate(ctx, t);
  return self ? qe::FromBool(dom::GetAttribute(self, Attribute).has_value()) : qe::Undefined();
}

template <const char* Attribute>
Value SetBoolean(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisTemplate(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  if (args[0].to_boolean()) dom::SetAttribute(ctx, self, Attribute, "");
  else dom::RemoveAttribute(self, Attribute);
  return qe::Undefined();
}

constexpr char kModeAttribute[] = "shadowrootmode";
constexpr char kDelegatesFocusAttribute[] = "shadowrootdelegatesfocus";
constexpr char kClonableAttribute[] = "shadowrootclonable";
constexpr char kSerializableAttribute[] = "shadowrootserializable";
constexpr char kSlotAssignmentAttribute[] = "shadowrootslotassignment";
constexpr char kOpen[] = "open";
constexpr char kClosed[] = "closed";
constexpr char kNone[] = "";
constexpr char kNamed[] = "named";
constexpr char kManual[] = "manual";

// What HTMLElement reflects of its content attributes.
dom::Element* ThisHtmlElement(Context& ctx, const Value& t) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (self && !self->IsHtml()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

template <const char* Attribute>
Value GetPlain(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  return self ? qe::FromWtf8(ctx, dom::GetAttribute(self, Attribute).value_or("")) : qe::Undefined();
}

template <const char* Attribute>
Value SetPlain(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, Attribute, std::move(text));
  return qe::Undefined();
}

template <const char* Attribute>
Value GetFlag(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  return self ? qe::FromBool(dom::GetAttribute(self, Attribute).has_value()) : qe::Undefined();
}

template <const char* Attribute>
Value SetFlag(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  if (args[0].to_boolean()) dom::SetAttribute(ctx, self, Attribute, "");
  else dom::RemoveAttribute(self, Attribute);
  return qe::Undefined();
}

// An enumerated attribute with two keywords, and what it is when it has neither.
template <const char* Attribute, const char* First, const char* Second, const char* Third>
Value GetKeyword(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self) return qe::Undefined();
  std::string value = dom::GetAttribute(self, Attribute).value_or("");
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  if (value == First || value == Second || (Third[0] && value == Third)) return qe::FromWtf8(ctx, value);
  return qe::FromWtf8(ctx, "");
}

// translate: "yes" and "no" say it, and anything else leaves it to the parent, down from the default of yes.
Value GetTranslate(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self) return qe::Undefined();
  for (dom::Node* node = self; node; node = node->parentNode) {
    const dom::Element* element = dom::AsElement(node);
    if (!element) continue;
    std::string value = dom::GetAttribute(element, "translate").value_or("");
    for (char& c : value) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    }
    if (value == "yes" || value == "") {
      if (dom::GetAttribute(element, "translate")) return qe::FromBool(true);
    } else if (value == "no") {
      return qe::FromBool(false);
    }
  }
  return qe::FromBool(true);
}

Value SetTranslate(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  dom::SetAttribute(ctx, self, "translate", args[0].to_boolean() ? "yes" : "no");
  return qe::Undefined();
}

// draggable and spellcheck: "true" and "false", and a default for what is neither.
template <const char* Attribute, bool Default>
Value GetBooleanKeyword(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self) return qe::Undefined();
  std::string value = dom::GetAttribute(self, Attribute).value_or("");
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  if (value == "true") return qe::FromBool(true);
  if (value == "false") return qe::FromBool(false);
  return qe::FromBool(Default);
}

template <const char* Attribute>
Value SetBooleanKeyword(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisHtmlElement(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  dom::SetAttribute(ctx, self, Attribute, args[0].to_boolean() ? "true" : "false");
  return qe::Undefined();
}

constexpr char kTitle[] = "title";
constexpr char kLang[] = "lang";
constexpr char kDir[] = "dir";
constexpr char kHidden[] = "hidden";
constexpr char kInert[] = "inert";
constexpr char kAccessKey[] = "accesskey";
constexpr char kAutocapitalize[] = "autocapitalize";
constexpr char kDraggable[] = "draggable";
constexpr char kSpellcheck[] = "spellcheck";
constexpr char kEnterKeyHint[] = "enterkeyhint";
constexpr char kInputMode[] = "inputmode";
constexpr char kLtr[] = "ltr";
constexpr char kRtl[] = "rtl";
constexpr char kAuto[] = "auto";

bool Flatten(Context& ctx, qe::Args args) {
  if (args.empty() || !qe::IsObject(args[0])) return false;
  Value flatten = qe::Get(ctx, args[0], "flatten");
  return !qe::HasException(ctx) && flatten.to_boolean();
}

Value AssignedNodes(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisSlot(ctx, t);
  if (!self) return qe::Undefined();
  const bool flatten = Flatten(ctx, args);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::vector<dom::Node*> nodes = flatten ? dom::FindFlattenedSlottables(self) : self->assignedNodes;
  Value array = qe::NewArray(ctx);
  for (dom::Node* node : nodes) qe::ArrayPush(ctx, array, qe::FromObject(node));
  return array;
}

Value AssignedElements(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisSlot(ctx, t);
  if (!self) return qe::Undefined();
  const bool flatten = Flatten(ctx, args);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::vector<dom::Node*> nodes = flatten ? dom::FindFlattenedSlottables(self) : self->assignedNodes;
  Value array = qe::NewArray(ctx);
  for (dom::Node* node : nodes) {
    if (node->IsElement()) qe::ArrayPush(ctx, array, qe::FromObject(node));
  }
  return array;
}

Value AssignMethod(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisSlot(ctx, t);
  if (!self) return qe::Undefined();
  std::vector<dom::Node*> nodes;
  for (const Value& arg : args) {
    dom::Node* node = Quanta::DOMObject::Cast<dom::Node>(arg);
    if (!node || !dom::IsSlottable(node)) {
      qe::ThrowTypeError(ctx, "Failed to execute 'assign' on 'HTMLSlotElement': The provided value is not of type '(Element or Text)'.");
      return qe::Undefined();
    }
    if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) nodes.push_back(node);
  }
  for (dom::Node* node : self->manuallyAssignedNodes) node->manualSlot = nullptr;
  std::vector<dom::Element*> previousSlots;
  for (dom::Node* node : nodes) {
    if (dom::Element* previous = node->manualSlot) {
      if (previous != self && std::find(previousSlots.begin(), previousSlots.end(), previous) == previousSlots.end()) previousSlots.push_back(previous);
      previous->manuallyAssignedNodes.erase(std::remove(previous->manuallyAssignedNodes.begin(), previous->manuallyAssignedNodes.end(), node), previous->manuallyAssignedNodes.end());
    }
  }
  self->manuallyAssignedNodes = nodes;
  for (dom::Node* node : nodes) node->manualSlot = self;
  self->NoteWrite();
  dom::AssignSlottablesForTree(self->Root());
  for (dom::Element* previous : previousSlots) dom::AssignSlottablesForSlot(previous);
  // What was asked for changed, whether or not it can be seen in the tree.
  dom::QueueSlotChange(self);
  return qe::Undefined();
}

// [HTMLConstructor]: an element interface's constructor makes an element of a custom element that extends it, so each
// has to know which interface it is the constructor of.
constexpr size_t kInterfaceCount = std::size(kInterfaces);
int g_constructorKeys[kInterfaceCount];

template <size_t I>
Value ConstructInterface(Context& ctx, Value, qe::Args, Value newTarget) {
  Object* active = static_cast<Object*>(qe::GetRealmData(ctx, &g_constructorKeys[I]));
  std::vector<std::string_view> names(kInterfaces[I].tags.begin(), kInterfaces[I].tags.end());
  return dom::ConstructHtmlElement(ctx, newTarget, active, names);
}

template <size_t... Is>
constexpr std::array<qe::NativeFn, sizeof...(Is)> MakeConstructors(std::index_sequence<Is...>) {
  return {ConstructInterface<Is>...};
}

const std::array<qe::NativeFn, kInterfaceCount> kConstructors = MakeConstructors(std::make_index_sequence<kInterfaceCount>());

}  // namespace

void DefineHtmlElementInterfaces(Context& ctx) {
  Object* htmlElement = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement);

  qe::ClassRef unknown = qe::DefineClass(ctx, "HTMLUnknownElement", IllegalConstructor, 0, htmlElement);
  qe::DefineGlobal(ctx, "HTMLUnknownElement", unknown.constructor);
  dom::SetUnknownHtmlElementInterface(ctx, unknown.prototype);

  qe::DefineAccessor(htmlElement, "title", GetPlain<kTitle>, dom::Reactions<SetPlain<kTitle>>);
  qe::DefineAccessor(htmlElement, "lang", GetPlain<kLang>, dom::Reactions<SetPlain<kLang>>);
  qe::DefineAccessor(htmlElement, "dir", GetKeyword<kDir, kLtr, kRtl, kAuto>, dom::Reactions<SetPlain<kDir>>);
  qe::DefineAccessor(htmlElement, "hidden", GetFlag<kHidden>, dom::Reactions<SetFlag<kHidden>>);
  qe::DefineAccessor(htmlElement, "inert", GetFlag<kInert>, dom::Reactions<SetFlag<kInert>>);
  qe::DefineAccessor(htmlElement, "accessKey", GetPlain<kAccessKey>, dom::Reactions<SetPlain<kAccessKey>>);
  qe::DefineAccessor(htmlElement, "autocapitalize", GetPlain<kAutocapitalize>, dom::Reactions<SetPlain<kAutocapitalize>>);
  qe::DefineAccessor(htmlElement, "enterKeyHint", GetPlain<kEnterKeyHint>, dom::Reactions<SetPlain<kEnterKeyHint>>);
  qe::DefineAccessor(htmlElement, "inputMode", GetPlain<kInputMode>, dom::Reactions<SetPlain<kInputMode>>);
  qe::DefineAccessor(htmlElement, "translate", GetTranslate, dom::Reactions<SetTranslate>);
  qe::DefineAccessor(htmlElement, "draggable", GetBooleanKeyword<kDraggable, false>, dom::Reactions<SetBooleanKeyword<kDraggable>>);
  qe::DefineAccessor(htmlElement, "spellcheck", GetBooleanKeyword<kSpellcheck, true>, dom::Reactions<SetBooleanKeyword<kSpellcheck>>);

  for (size_t index = 0; index < kInterfaceCount; ++index) {
    const Interface& interface = kInterfaces[index];
    qe::ClassRef definition = qe::DefineClass(ctx, interface.name, kConstructors[index], 0, htmlElement);
    qe::SetRealmData(ctx, &g_constructorKeys[index], definition.constructor);
    qe::DefineGlobal(ctx, interface.name, definition.constructor);
    for (const char* tag : interface.tags) dom::RegisterHtmlElementInterface(ctx, tag, definition.prototype);
    if (std::string_view(interface.name) == "HTMLTemplateElement") {
      qe::DefineAccessor(definition.prototype, "content", GetTemplateContent, nullptr);
      qe::DefineAccessor(definition.prototype, "shadowRootMode", GetEnumerated<kModeAttribute, kOpen, kClosed, kNone>, SetString<kModeAttribute>);
      qe::DefineAccessor(definition.prototype, "shadowRootDelegatesFocus", GetBoolean<kDelegatesFocusAttribute>, SetBoolean<kDelegatesFocusAttribute>);
      qe::DefineAccessor(definition.prototype, "shadowRootClonable", GetBoolean<kClonableAttribute>, SetBoolean<kClonableAttribute>);
      qe::DefineAccessor(definition.prototype, "shadowRootSerializable", GetBoolean<kSerializableAttribute>, SetBoolean<kSerializableAttribute>);
      qe::DefineAccessor(definition.prototype, "shadowRootSlotAssignment", GetEnumerated<kSlotAssignmentAttribute, kNamed, kManual, kNamed>, SetString<kSlotAssignmentAttribute>);
    }
    if (std::string_view(interface.name) == "HTMLIFrameElement") DefineIframeMembers(ctx, definition.prototype);
    if (std::string_view(interface.name) == "HTMLScriptElement") DefineScriptMembers(ctx, definition.prototype);
    if (std::string_view(interface.name) == "HTMLSlotElement") {
      qe::DefineAccessor(definition.prototype, "name", GetSlotName, SetSlotName);
      qe::DefineMethod(definition.prototype, "assignedNodes", AssignedNodes, 0);
      qe::DefineMethod(definition.prototype, "assignedElements", AssignedElements, 0);
      qe::DefineMethod(definition.prototype, "assign", AssignMethod, 0);
    }
  }
  for (const char* tag : kPlainTags) dom::RegisterHtmlElementInterface(ctx, tag, htmlElement);
}

}  // namespace solar::html
