#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/dom/Range.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_characterDataKey;

bool Missing(Context& ctx, qe::Args args, size_t count, const char* interface, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on '" + interface + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return true;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

// "replace data": false, with the error raised, if the offset is past the end.
bool ReplaceData(Context& ctx, CharacterData* node, uint32_t offset, uint32_t count, std::string_view replacement) {
  if (auto error = dom::ReplaceData(node, offset, count, replacement)) {
    Throw(ctx, *error);
    return false;
  }
  return true;
}

Value GetData(Context& ctx, Value t, qe::Args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  return self ? qe::FromWtf8(ctx, self->data) : qe::Undefined();
}

Value SetData(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self) return qe::Undefined();
  std::string text;
  if (!args.empty() && !qe::IsNull(args[0])) {
    text = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  SetCharacterData(self, std::move(text));
  return qe::Undefined();
}

Value GetLength(Context& ctx, Value t, qe::Args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(ToUtf16(self->data).size())) : qe::Undefined();
}

Value SubstringData(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || Missing(ctx, args, 2, "CharacterData", "substringData")) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[0]);
  const uint32_t count = qe::ToUint32(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::u16string units = ToUtf16(self->data);
  if (offset > units.size()) {
    Throw(ctx, {"IndexSizeError", "The offset " + std::to_string(offset) + " is larger than the node's length (" + std::to_string(units.size()) + ")."});
    return qe::Undefined();
  }
  return qe::FromWtf8(ctx, FromUtf16(std::u16string_view(units).substr(offset, count)));
}

Value AppendData(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || Missing(ctx, args, 1, "CharacterData", "appendData")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  AppendCharacterData(self, text);
  return qe::Undefined();
}

Value InsertData(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || Missing(ctx, args, 2, "CharacterData", "insertData")) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[0]);
  const std::string text = qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  ReplaceData(ctx, self, offset, 0, text);
  return qe::Undefined();
}

Value DeleteData(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || Missing(ctx, args, 2, "CharacterData", "deleteData")) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[0]);
  const uint32_t count = qe::ToUint32(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  ReplaceData(ctx, self, offset, count, "");
  return qe::Undefined();
}

Value ReplaceDataMethod(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || Missing(ctx, args, 3, "CharacterData", "replaceData")) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[0]);
  const uint32_t count = qe::ToUint32(ctx, args[1]);
  const std::string text = qe::ToWtf8(ctx, args[2]);
  if (qe::HasException(ctx)) return qe::Undefined();
  ReplaceData(ctx, self, offset, count, text);
  return qe::Undefined();
}

// ---- Text, Comment ----

template <CharacterData* (*Make)(Context&, Document*, std::string), Object* (*Proto)(Context&), const char* Name>
Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, std::string("Failed to construct '") + Name + "': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::string data;
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    data = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  CharacterData* node = Make(ctx, AssociatedDocument(ctx), std::move(data));
  if (prototype) node->initialize_prototype(prototype);
  return qe::FromObject(node);
}

constexpr char kTextName[] = "Text";
constexpr char kCommentName[] = "Comment";

Object* TextPrototype(Context& ctx) { return InterfacePrototype(ctx, Interface::Text); }
Object* CommentPrototype(Context& ctx) { return InterfacePrototype(ctx, Interface::Comment); }

Value SplitText(Context& ctx, Value t, qe::Args args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || !self->IsText() || Missing(ctx, args, 1, "Text", "splitText")) {
    if (self && !qe::HasException(ctx)) qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  const uint32_t offset = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::u16string units = ToUtf16(self->data);
  if (offset > units.size()) {
    Throw(ctx, {"IndexSizeError", "The offset " + std::to_string(offset) + " is larger than the node's length (" + std::to_string(units.size()) + ")."});
    return qe::Undefined();
  }
  const uint32_t count = static_cast<uint32_t>(units.size() - offset);
  CharacterData* created = NewText(ctx, self->nodeDocument, FromUtf16(std::u16string_view(units).substr(offset, count)));
  if (self->parentNode) {
    InsertUnchecked(created, self->parentNode, self->nextSibling);
    RangesSplitText(self, created, offset);
  }
  ReplaceData(ctx, self, offset, count, "");
  return qe::FromObject(created);
}

Value GetWholeText(Context& ctx, Value t, qe::Args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || !self->IsText()) {
    if (self) qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  Node* first = self;
  while (first->previousSibling && first->previousSibling->IsText()) first = first->previousSibling;
  std::string text;
  for (Node* node = first; node && node->IsText(); node = node->nextSibling) text += static_cast<CharacterData*>(node)->data;
  return qe::FromWtf8(ctx, text);
}

Value GetTarget(Context& ctx, Value t, qe::Args, Value) {
  CharacterData* self = ThisCharacterData(ctx, t);
  if (!self || self->nodeType != NodeType::ProcessingInstruction) {
    if (self) qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return qe::FromWtf8(ctx, self->target);
}

// ---- DocumentType, DocumentFragment, Attr ----

DocumentType* ThisDoctype(Context& ctx, const Value& t) {
  Node* node = ThisNode(ctx, t);
  if (node && !node->IsDocumentType()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<DocumentType*>(node);
}

Value GetDoctypeName(Context& ctx, Value t, qe::Args, Value) {
  DocumentType* self = ThisDoctype(ctx, t);
  return self ? qe::FromWtf8(ctx, self->name) : qe::Undefined();
}
Value GetPublicId(Context& ctx, Value t, qe::Args, Value) {
  DocumentType* self = ThisDoctype(ctx, t);
  return self ? qe::FromWtf8(ctx, self->publicId) : qe::Undefined();
}
Value GetSystemId(Context& ctx, Value t, qe::Args, Value) {
  DocumentType* self = ThisDoctype(ctx, t);
  return self ? qe::FromWtf8(ctx, self->systemId) : qe::Undefined();
}

Value ConstructFragment(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'DocumentFragment': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  DocumentFragment* fragment = NewDocumentFragment(ctx, AssociatedDocument(ctx));
  if (prototype) fragment->initialize_prototype(prototype);
  return qe::FromObject(fragment);
}

Attr* ThisAttr(Context& ctx, const Value& t) {
  Node* node = ThisNode(ctx, t);
  if (node && node->nodeType != NodeType::Attribute) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<Attr*>(node);
}

Value NullableUtf8(Context& ctx, const std::string& text) { return text.empty() ? qe::Null() : qe::FromWtf8(ctx, text); }

Value GetAttrNamespace(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? NullableUtf8(ctx, self->namespaceUri) : qe::Undefined();
}
Value GetAttrPrefix(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? NullableUtf8(ctx, self->prefix) : qe::Undefined();
}
Value GetAttrLocalName(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? qe::FromWtf8(ctx, self->localName) : qe::Undefined();
}
Value GetAttrName(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? qe::FromWtf8(ctx, self->QualifiedName()) : qe::Undefined();
}
Value GetAttrValue(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? qe::FromWtf8(ctx, self->value) : qe::Undefined();
}
Value SetAttrValue(Context& ctx, Value t, qe::Args args, Value) {
  Attr* self = ThisAttr(ctx, t);
  if (!self || Missing(ctx, args, 1, "Attr", "value")) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  SetAttrValue(self, std::move(text));
  return qe::Undefined();
}
Value GetOwnerElement(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? NodeValue(self->ownerElement) : qe::Undefined();
}
Value GetSpecified(Context& ctx, Value t, qe::Args, Value) {
  Attr* self = ThisAttr(ctx, t);
  return self ? qe::FromBool(true) : qe::Undefined();
}

}  // namespace

void DefineCharacterDataClasses(Context& ctx) {
  Object* nodeProto = NodePrototype(ctx);

  qe::ClassRef characterData = qe::DefineClass(ctx, "CharacterData", IllegalConstructor, 0, nodeProto);
  qe::SetRealmData(ctx, &g_characterDataKey, characterData.prototype);
  Object* cd = characterData.prototype;
  qe::DefineAccessor(cd, "data", GetData, SetData);
  qe::DefineAccessor(cd, "length", GetLength, nullptr);
  qe::DefineMethod(cd, "substringData", SubstringData, 2);
  qe::DefineMethod(cd, "appendData", AppendData, 1);
  qe::DefineMethod(cd, "insertData", InsertData, 2);
  qe::DefineMethod(cd, "deleteData", DeleteData, 2);
  qe::DefineMethod(cd, "replaceData", ReplaceDataMethod, 3);
  DefineChildNode(cd);
  DefineNonDocumentTypeChildNode(cd);
  qe::DefineGlobal(ctx, "CharacterData", characterData.constructor);

  qe::ClassRef text = qe::DefineClass(ctx, "Text", Construct<NewText, TextPrototype, kTextName>, 0, cd);
  SetInterfacePrototype(ctx, Interface::Text, text.prototype);
  qe::DefineMethod(text.prototype, "splitText", SplitText, 1);
  qe::DefineAccessor(text.prototype, "wholeText", GetWholeText, nullptr);
  qe::DefineGlobal(ctx, "Text", text.constructor);

  qe::ClassRef cdata = qe::DefineClass(ctx, "CDATASection", IllegalConstructor, 0, text.prototype);
  SetInterfacePrototype(ctx, Interface::CdataSection, cdata.prototype);
  qe::DefineGlobal(ctx, "CDATASection", cdata.constructor);

  qe::ClassRef instruction = qe::DefineClass(ctx, "ProcessingInstruction", IllegalConstructor, 0, cd);
  SetInterfacePrototype(ctx, Interface::ProcessingInstruction, instruction.prototype);
  qe::DefineAccessor(instruction.prototype, "target", GetTarget, nullptr);
  qe::DefineGlobal(ctx, "ProcessingInstruction", instruction.constructor);

  qe::ClassRef comment = qe::DefineClass(ctx, "Comment", Construct<NewComment, CommentPrototype, kCommentName>, 0, cd);
  SetInterfacePrototype(ctx, Interface::Comment, comment.prototype);
  qe::DefineGlobal(ctx, "Comment", comment.constructor);

  qe::ClassRef doctype = qe::DefineClass(ctx, "DocumentType", IllegalConstructor, 0, nodeProto);
  SetInterfacePrototype(ctx, Interface::DocumentType, doctype.prototype);
  qe::DefineAccessor(doctype.prototype, "name", GetDoctypeName, nullptr);
  qe::DefineAccessor(doctype.prototype, "publicId", GetPublicId, nullptr);
  qe::DefineAccessor(doctype.prototype, "systemId", GetSystemId, nullptr);
  DefineChildNode(doctype.prototype);
  qe::DefineGlobal(ctx, "DocumentType", doctype.constructor);

  qe::ClassRef fragment = qe::DefineClass(ctx, "DocumentFragment", ConstructFragment, 0, nodeProto);
  SetInterfacePrototype(ctx, Interface::DocumentFragment, fragment.prototype);
  DefineParentNode(fragment.prototype);
  qe::DefineGlobal(ctx, "DocumentFragment", fragment.constructor);

  qe::ClassRef attr = qe::DefineClass(ctx, "Attr", IllegalConstructor, 0, nodeProto);
  SetInterfacePrototype(ctx, Interface::Attr, attr.prototype);
  qe::DefineAccessor(attr.prototype, "namespaceURI", GetAttrNamespace, nullptr);
  qe::DefineAccessor(attr.prototype, "prefix", GetAttrPrefix, nullptr);
  qe::DefineAccessor(attr.prototype, "localName", GetAttrLocalName, nullptr);
  qe::DefineAccessor(attr.prototype, "name", GetAttrName, nullptr);
  qe::DefineAccessor(attr.prototype, "value", GetAttrValue, Reactions<SetAttrValue>);
  qe::DefineAccessor(attr.prototype, "ownerElement", GetOwnerElement, nullptr);
  qe::DefineAccessor(attr.prototype, "specified", GetSpecified, nullptr);
  qe::DefineGlobal(ctx, "Attr", attr.constructor);
}

}  // namespace solar::dom
