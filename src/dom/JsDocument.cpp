#include <algorithm>
#include <string>
#include <unordered_map>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_holderKey;
char g_implementationKey;

// The class the realm's associated Document is kept on: a prototype lives as long as the realm and is
// traced, and a class that is never made global is out of the page's reach.
Value HolderConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Object* Holder(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_holderKey)); }

// The interfaces of HTML elements by name, kept on the holder where the collector sees them.
char g_htmlTableKey;

struct HtmlTable : DOMObject {
  std::unordered_map<std::string, Object*> byTag;
  Object* unknown = nullptr;
  void Visit(Quanta::Visitor& visitor) {
    for (auto& entry : byTag) visitor.Mark(entry.second);
    visitor.Mark(unknown);
  }
};

HtmlTable* TableOf(Context& ctx, bool create) {
  if (auto* table = static_cast<HtmlTable*>(qe::GetRealmData(ctx, &g_htmlTableKey))) return table;
  Object* holder = Holder(ctx);
  if (!create || !holder) return nullptr;
  HtmlTable* table = Heap::Allocate<HtmlTable>();
  table->initialize_prototype(nullptr);
  qe::Set(ctx, qe::FromObject(holder), "htmlTable", qe::FromObject(table));
  qe::SetRealmData(ctx, &g_htmlTableKey, table);
  return table;
}


bool Missing(Context& ctx, qe::Args args, size_t count, const char* interface, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on '" + interface + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return true;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value ConstructDocument(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Document': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  Document* document = NewDocument(ctx, false);
  if (prototype) document->initialize_prototype(prototype);
  return qe::FromObject(document);
}

Value GetUrl(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromWtf8(ctx, self->url) : qe::Undefined();
}

Value GetCompatMode(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromWtf8(ctx, self->mode == Document::Mode::Quirks ? "BackCompat" : "CSS1Compat") : qe::Undefined();
}

Value GetCharacterSet(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromWtf8(ctx, self->characterSet) : qe::Undefined();
}

Value GetContentType(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromWtf8(ctx, self->contentType) : qe::Undefined();
}

Value GetDoctype(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? NodeValue(self->Doctype()) : qe::Undefined();
}

Value GetDocumentElement(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? NodeValue(self->DocumentElement()) : qe::Undefined();
}

Value GetElementsByTagNameDocument(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "getElementsByTagName")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByTagName(ctx, self, name);
}

Value GetElementsByTagNameNsDocument(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 2, "Document", "getElementsByTagNameNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByTagNameNS(ctx, self, ns.value_or(""), local);
}

Value GetElementsByClassNameDocument(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "getElementsByClassName")) return qe::Undefined();
  const std::string classes = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByClassName(ctx, self, classes);
}

Value CreateElement(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "createElement")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!IsXmlName(name)) {
    Throw(ctx, {"InvalidCharacterError", "The tag name provided ('" + name + "') is not a valid name."});
    return qe::Undefined();
  }
  if (self->isHtml) name = Lower(name);
  // An HTML document, or an XHTML one, makes HTML elements; another XML document makes elements of no namespace.
  const bool html = self->isHtml || self->contentType == "application/xhtml+xml";
  return qe::FromObject(NewElement(ctx, self, name, html ? kHtmlNamespace : std::string_view()));
}

Value CreateElementNs(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 2, "Document", "createElementNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string qualified = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  QualifiedParts parts;
  if (auto error = ValidateAndExtract(ns.value_or(""), qualified, parts)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(NewElement(ctx, self, parts.localName, parts.namespaceUri, parts.prefix));
}

Value CreateDocumentFragment(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromObject(NewDocumentFragment(ctx, self)) : qe::Undefined();
}

Value CreateTextNode(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "createTextNode")) return qe::Undefined();
  std::string data = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : qe::FromObject(NewText(ctx, self, std::move(data)));
}

Value CreateComment(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "createComment")) return qe::Undefined();
  std::string data = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : qe::FromObject(NewComment(ctx, self, std::move(data)));
}

Value CreateCdataSection(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "createCDATASection")) return qe::Undefined();
  std::string data = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (self->isHtml) {
    Throw(ctx, {"NotSupportedError", "This operation is not supported for HTML documents"});
    return qe::Undefined();
  }
  if (data.find("]]>") != std::string::npos) {
    Throw(ctx, {"InvalidCharacterError", "String contains an invalid character"});
    return qe::Undefined();
  }
  return qe::FromObject(NewCdataSection(ctx, self, std::move(data)));
}

Value CreateProcessingInstruction(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 2, "Document", "createProcessingInstruction")) return qe::Undefined();
  std::string target = qe::ToWtf8(ctx, args[0]);
  std::string data = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!IsXmlName(target) || data.find("?>") != std::string::npos) {
    Throw(ctx, {"InvalidCharacterError", "String contains an invalid character"});
    return qe::Undefined();
  }
  return qe::FromObject(NewProcessingInstruction(ctx, self, std::move(target), std::move(data)));
}

Value CreateAttribute(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "createAttribute")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!IsXmlName(name)) {
    Throw(ctx, {"InvalidCharacterError", "String contains an invalid character"});
    return qe::Undefined();
  }
  if (self->isHtml) name = Lower(name);
  return qe::FromObject(NewAttr(ctx, self, "", "", name, ""));
}

Value CreateAttributeNs(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 2, "Document", "createAttributeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string qualified = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  QualifiedParts parts;
  if (auto error = ValidateAndExtract(ns.value_or(""), qualified, parts)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(NewAttr(ctx, self, parts.namespaceUri, parts.prefix, parts.localName, ""));
}

Value ImportNode(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "importNode")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'importNode' on 'Document'");
  if (!node) return qe::Undefined();
  if (node->IsDocument()) {
    Throw(ctx, {"NotSupportedError", "The node provided is a document, which may not be imported"});
    return qe::Undefined();
  }
  Node* clone = CloneNode(ctx, node, args.size() > 1 && args[1].to_boolean());
  Adopt(clone, self);
  return qe::FromObject(clone);
}

Value AdoptNode(Context& ctx, Value t, qe::Args args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "adoptNode")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'adoptNode' on 'Document'");
  if (!node) return qe::Undefined();
  if (node->IsDocument()) {
    Throw(ctx, {"NotSupportedError", "The node provided is a document, which may not be adopted"});
    return qe::Undefined();
  }
  Adopt(node, self);
  return qe::FromObject(node);
}

Element* FindById(Node* root, const std::string& id) {
  for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
    Element* element = AsElement(node);
    if (!element) continue;
    const Attr* attribute = element->FindAttribute("", "id");
    if (attribute && attribute->value == id) return element;
  }
  return nullptr;
}

Value GetElementById(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "getElementById")) return qe::Undefined();
  const std::string id = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return id.empty() ? qe::Null() : NodeValue(FindById(self, id));
}

// ---- DOMImplementation ----

struct JsImplementation : DOMObject {
  Document* document = nullptr;
  void Visit(Quanta::Visitor& visitor) { visitor.Mark(document); }
};

JsImplementation* ThisImplementation(Context& ctx, const Value& t) {
  JsImplementation* self = DOMObject::Cast<JsImplementation>(t);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value CreateDocumentType(Context& ctx, Value t, qe::Args args, Value) {
  JsImplementation* self = ThisImplementation(ctx, t);
  if (!self || Missing(ctx, args, 3, "DOMImplementation", "createDocumentType")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  std::string publicId = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  std::string systemId = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[2]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (auto error = ValidateQualifiedName(name)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(NewDocumentType(ctx, self->document, std::move(name), std::move(publicId), std::move(systemId)));
}

Value CreateDocument(Context& ctx, Value t, qe::Args args, Value) {
  JsImplementation* self = ThisImplementation(ctx, t);
  if (!self || Missing(ctx, args, 2, "DOMImplementation", "createDocument")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string qualified = qe::HasException(ctx) ? "" : (qe::IsNull(args[1]) ? "" : qe::ToWtf8(ctx, args[1]));
  if (qe::HasException(ctx)) return qe::Undefined();
  Node* doctype = nullptr;
  if (args.size() > 2 && !qe::IsNull(args[2]) && !qe::IsUndefined(args[2])) {
    doctype = DOMObject::Cast<Node>(args[2]);
    if (!doctype || !doctype->IsDocumentType()) {
      qe::ThrowTypeError(ctx, "Failed to execute 'createDocument' on 'DOMImplementation': parameter 3 is not of type 'DocumentType'.");
      return qe::Undefined();
    }
  }
  Document* document = NewDocument(ctx, false);
  const std::string type = ns == std::string(kHtmlNamespace) ? "application/xhtml+xml" : ns == std::string(kSvgNamespace) ? "image/svg+xml" : "application/xml";
  document->contentType = type;
  if (doctype) AppendChild(document, doctype);
  if (!qualified.empty()) {
    QualifiedParts parts;
    if (auto error = ValidateAndExtract(ns.value_or(""), qualified, parts)) {
      Throw(ctx, *error);
      return qe::Undefined();
    }
    AppendChild(document, NewElement(ctx, document, parts.localName, parts.namespaceUri, parts.prefix));
  }
  return qe::FromObject(document);
}

Value CreateHtmlDocument(Context& ctx, Value t, qe::Args args, Value) {
  JsImplementation* self = ThisImplementation(ctx, t);
  if (!self) return qe::Undefined();
  std::optional<std::string> title;
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    title = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  Document* document = NewDocument(ctx, true);
  AppendChild(document, NewDocumentType(ctx, document, "html", "", ""));
  Element* html = NewElement(ctx, document, "html");
  AppendChild(document, html);
  AppendChild(html, NewElement(ctx, document, "head"));
  if (title) {
    Element* titleElement = NewElement(ctx, document, "title");
    AppendChild(titleElement, NewText(ctx, document, *title));
    AppendChild(html->firstChild, titleElement);
  }
  AppendChild(html, NewElement(ctx, document, "body"));
  return qe::FromObject(document);
}

Value HasFeature(Context& ctx, Value t, qe::Args, Value) {
  return ThisImplementation(ctx, t) ? qe::FromBool(true) : qe::Undefined();
}

Value GetImplementationOf(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->implementation) {
    JsImplementation* implementation = Heap::Allocate<JsImplementation>();
    implementation->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_implementationKey)));
    implementation->document = self;
    self->implementation = implementation;
    self->NoteWrite();
  }
  return qe::FromObject(self->implementation);
}

}  // namespace

Object* RealmHolder(Context& ctx) { return Holder(ctx); }

bool HasWindowName(const Element* element, const std::string& name) {
  if (const Attr* id = element->FindAttribute("", "id"); id && id->value == name) return true;
  if (element->IsHtml() && (element->localName == "embed" || element->localName == "form" || element->localName == "img" || element->localName == "object")) {
    if (const Attr* attribute = element->FindAttribute("", "name"); attribute && attribute->value == name) return true;
  }
  return false;
}

// The window's script keeps its own record of which names are properties of it; it is told of each that changes.
void WindowNamesChanged(Context& ctx, const std::string& name) {
  Object* holder = Holder(ctx);
  if (!holder) return;
  Value update = qe::Get(ctx, qe::FromObject(holder), "windowNamedUpdate");
  if (!qe::IsCallable(update)) return;
  Value argument = qe::FromWtf8(ctx, name);
  qe::Call(ctx, update, qe::Undefined(), qe::Args(&argument, 1));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

Object* HtmlElementPrototype(Context& ctx, std::string_view localName) {
  HtmlTable* table = TableOf(ctx, false);
  if (!table) return nullptr;
  const auto found = table->byTag.find(std::string(localName));
  if (found != table->byTag.end()) return found->second;
  if (IsValidCustomElementName(localName)) return nullptr;  // HTMLElement itself
  return table->unknown;
}

void RegisterHtmlElementInterface(Context& ctx, std::string_view localName, Object* prototype) {
  if (HtmlTable* table = TableOf(ctx, true)) table->byTag[std::string(localName)] = prototype;
}

void SetUnknownHtmlElementInterface(Context& ctx, Object* prototype) {
  if (HtmlTable* table = TableOf(ctx, true)) table->unknown = prototype;
}

Document* AssociatedDocument(Context& ctx) {
  Object* holder = Holder(ctx);
  if (!holder) return nullptr;
  Value existing = qe::Get(ctx, qe::FromObject(holder), "document");
  if (Document* document = DOMObject::Cast<Document>(existing)) return document;
  Document* document = NewDocument(ctx, true);
  qe::Set(ctx, qe::FromObject(holder), "document", qe::FromObject(document));
  return document;
}

void SetAssociatedDocument(Context& ctx, Document* document) {
  if (Object* holder = Holder(ctx)) qe::Set(ctx, qe::FromObject(holder), "document", qe::FromObject(document));
}

void DefineDocumentClasses(Context& ctx) {
  qe::ClassRef holder = qe::DefineClass(ctx, "SolarDomHolder", HolderConstructor, 0);
  qe::SetRealmData(ctx, &g_holderKey, holder.prototype);

  qe::ClassRef implementation = qe::DefineClass(ctx, "DOMImplementation", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_implementationKey, implementation.prototype);
  qe::DefineMethod(implementation.prototype, "createDocumentType", CreateDocumentType, 3);
  qe::DefineMethod(implementation.prototype, "createDocument", CreateDocument, 2);
  qe::DefineMethod(implementation.prototype, "createHTMLDocument", CreateHtmlDocument, 0);
  qe::DefineMethod(implementation.prototype, "hasFeature", HasFeature, 0);
  qe::DefineGlobal(ctx, "DOMImplementation", implementation.constructor);

  qe::ClassRef document = qe::DefineClass(ctx, "Document", ConstructDocument, 0, NodePrototype(ctx));
  SetInterfacePrototype(ctx, Interface::Document, document.prototype);
  Object* p = document.prototype;
  qe::DefineAccessor(p, "implementation", GetImplementationOf, nullptr);
  qe::DefineAccessor(p, "URL", GetUrl, nullptr);
  qe::DefineAccessor(p, "documentURI", GetUrl, nullptr);
  qe::DefineAccessor(p, "compatMode", GetCompatMode, nullptr);
  qe::DefineAccessor(p, "characterSet", GetCharacterSet, nullptr);
  qe::DefineAccessor(p, "charset", GetCharacterSet, nullptr);
  qe::DefineAccessor(p, "inputEncoding", GetCharacterSet, nullptr);
  qe::DefineAccessor(p, "contentType", GetContentType, nullptr);
  qe::DefineAccessor(p, "doctype", GetDoctype, nullptr);
  qe::DefineAccessor(p, "documentElement", GetDocumentElement, nullptr);
  qe::DefineMethod(p, "getElementsByTagName", GetElementsByTagNameDocument, 1);
  qe::DefineMethod(p, "getElementsByTagNameNS", GetElementsByTagNameNsDocument, 2);
  qe::DefineMethod(p, "getElementsByClassName", GetElementsByClassNameDocument, 1);
  qe::DefineMethod(p, "createElement", CreateElement, 1);
  qe::DefineMethod(p, "createElementNS", CreateElementNs, 2);
  qe::DefineMethod(p, "createDocumentFragment", CreateDocumentFragment, 0);
  qe::DefineMethod(p, "createTextNode", CreateTextNode, 1);
  qe::DefineMethod(p, "createCDATASection", CreateCdataSection, 1);
  qe::DefineMethod(p, "createComment", CreateComment, 1);
  qe::DefineMethod(p, "createProcessingInstruction", CreateProcessingInstruction, 2);
  qe::DefineMethod(p, "importNode", ImportNode, 1);
  qe::DefineMethod(p, "adoptNode", AdoptNode, 1);
  qe::DefineMethod(p, "createAttribute", CreateAttribute, 1);
  qe::DefineMethod(p, "createAttributeNS", CreateAttributeNs, 2);
  qe::DefineMethod(p, "getElementById", GetElementById, 1);
  DefineParentNode(p);
  qe::DefineGlobal(ctx, "Document", document.constructor);
  // The fragment's getElementById, which the standard gives DocumentFragment (a NonElementParentNode too).
  if (Object* fragment = InterfacePrototype(ctx, Interface::DocumentFragment)) qe::DefineMethod(fragment, "getElementById", GetElementById, 1);
}

}  // namespace solar::dom
