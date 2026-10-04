#include <string>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/dom/Range.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_rangeKey;
char g_staticRangeKey;

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

Range* ThisRange(Context& ctx, const Value& t) {
  Range* range = DOMObject::Cast<Range>(t);
  if (!range) qe::ThrowTypeError(ctx, "Illegal invocation");
  return range;
}

Range* LiveRange(Context& ctx, const Value& t) {
  Range* range = ThisRange(ctx, t);
  if (range && range->isStatic) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return range;
}

Value ConstructRange(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Range': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  Range* range = NewRange(ctx, AssociatedDocument(ctx));
  if (prototype) range->initialize_prototype(prototype);
  return qe::FromObject(range);
}

Value GetStartContainer(Context& ctx, Value t, qe::Args, Value) {
  Range* self = ThisRange(ctx, t);
  return self ? NodeValue(self->startNode) : qe::Undefined();
}
Value GetEndContainer(Context& ctx, Value t, qe::Args, Value) {
  Range* self = ThisRange(ctx, t);
  return self ? NodeValue(self->endNode) : qe::Undefined();
}
Value GetStartOffset(Context& ctx, Value t, qe::Args, Value) {
  Range* self = ThisRange(ctx, t);
  return self ? qe::FromUint32(self->startOffset) : qe::Undefined();
}
Value GetEndOffset(Context& ctx, Value t, qe::Args, Value) {
  Range* self = ThisRange(ctx, t);
  return self ? qe::FromUint32(self->endOffset) : qe::Undefined();
}
Value GetCollapsed(Context& ctx, Value t, qe::Args, Value) {
  Range* self = ThisRange(ctx, t);
  return self ? qe::FromBool(self->Collapsed()) : qe::Undefined();
}

Value GetCommonAncestor(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self) return qe::Undefined();
  Node* node = self->startNode;
  while (!node->Contains(self->endNode)) node = node->parentNode;
  return qe::FromObject(node);
}

// A node argument, or null with a TypeError pending.
Node* NodeAt(Context& ctx, qe::Args args, size_t index, const char* member) {
  return NodeArgument(ctx, args, index, (std::string("Failed to execute '") + member + "' on 'Range'").c_str());
}

bool Boundary(Context& ctx, Range* range, bool start, qe::Args args, const char* member) {
  if (Missing(ctx, args, 2, "Range", member)) return false;
  Node* node = NodeAt(ctx, args, 0, member);
  if (!node) return false;
  const uint32_t offset = qe::ToUint32(ctx, args[1]);
  if (qe::HasException(ctx)) return false;
  if (auto error = SetBoundary(range, start, node, offset)) {
    Throw(ctx, *error);
    return false;
  }
  return true;
}

Value SetStart(Context& ctx, Value t, qe::Args args, Value) {
  if (Range* self = LiveRange(ctx, t)) Boundary(ctx, self, true, args, "setStart");
  return qe::Undefined();
}
Value SetEnd(Context& ctx, Value t, qe::Args args, Value) {
  if (Range* self = LiveRange(ctx, t)) Boundary(ctx, self, false, args, "setEnd");
  return qe::Undefined();
}

// setStartBefore and the other three: a boundary point next to a node.
template <bool Start, bool After>
Value SetNextTo(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", Start ? (After ? "setStartAfter" : "setStartBefore") : (After ? "setEndAfter" : "setEndBefore"))) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, Start ? "setStartBefore" : "setEndBefore");
  if (!node) return qe::Undefined();
  Node* parent = node->parentNode;
  if (!parent) {
    Throw(ctx, {"InvalidNodeTypeError", "The node has no parent"});
    return qe::Undefined();
  }
  if (auto error = SetBoundary(self, Start, parent, static_cast<uint32_t>(node->IndexInParent()) + (After ? 1 : 0))) Throw(ctx, *error);
  return qe::Undefined();
}

Value Collapse(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self) return qe::Undefined();
  if (!args.empty() && args[0].to_boolean()) {
    self->endNode = self->startNode;
    self->endOffset = self->startOffset;
  } else {
    self->startNode = self->endNode;
    self->startOffset = self->endOffset;
  }
  self->NoteWrite();
  return qe::Undefined();
}

Value SelectNode(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", "selectNode")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "selectNode");
  if (!node) return qe::Undefined();
  Node* parent = node->parentNode;
  if (!parent) {
    Throw(ctx, {"InvalidNodeTypeError", "The node has no parent"});
    return qe::Undefined();
  }
  const uint32_t index = static_cast<uint32_t>(node->IndexInParent());
  self->startNode = self->endNode = parent;
  self->startOffset = index;
  self->endOffset = index + 1;
  self->NoteWrite();
  return qe::Undefined();
}

Value SelectNodeContents(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", "selectNodeContents")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "selectNodeContents");
  if (!node) return qe::Undefined();
  if (node->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The node is a doctype"});
    return qe::Undefined();
  }
  self->startNode = self->endNode = node;
  self->startOffset = 0;
  self->endOffset = NodeLength(node);
  self->NoteWrite();
  return qe::Undefined();
}

// A number the standard returns as a short: -1, 0 or 1 are what script sees.
Value Signed(int n) { return Value(static_cast<double>(n)); }

Value CompareBoundaryPoints(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 2, "Range", "compareBoundaryPoints")) return qe::Undefined();
  // An unsigned short: the number modulo 2^16.
  const uint32_t how = qe::ToUint32(ctx, args[0]) & 0xFFFF;
  Range* source = DOMObject::Cast<Range>(args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!source || source->isStatic) {
    qe::ThrowTypeError(ctx, "Failed to execute 'compareBoundaryPoints' on 'Range': parameter 2 is not of type 'Range'.");
    return qe::Undefined();
  }
  if (how > 3) {
    Throw(ctx, {"NotSupportedError", "The comparison method provided must be one of 'START_TO_START', 'START_TO_END', 'END_TO_END', or 'END_TO_START'."});
    return qe::Undefined();
  }
  if (self->startNode->Root() != source->startNode->Root()) {
    Throw(ctx, {"WrongDocumentError", "The two ranges are in different trees"});
    return qe::Undefined();
  }
  Node *thisNode, *otherNode;
  uint32_t thisOffset, otherOffset;
  switch (how) {
    case 0: thisNode = self->startNode; thisOffset = self->startOffset; otherNode = source->startNode; otherOffset = source->startOffset; break;
    case 1: thisNode = self->endNode; thisOffset = self->endOffset; otherNode = source->startNode; otherOffset = source->startOffset; break;
    case 2: thisNode = self->endNode; thisOffset = self->endOffset; otherNode = source->endNode; otherOffset = source->endOffset; break;
    default: thisNode = self->startNode; thisOffset = self->startOffset; otherNode = source->endNode; otherOffset = source->endOffset; break;
  }
  const int result = ComparePoints(thisNode, thisOffset, otherNode, otherOffset);
  return Signed(result);
}

Value ComparePointMethod(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 2, "Range", "comparePoint")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "comparePoint");
  if (!node) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (node->Root() != self->startNode->Root()) {
    Throw(ctx, {"WrongDocumentError", "The node is in another tree than the range"});
    return qe::Undefined();
  }
  if (node->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The node is a doctype"});
    return qe::Undefined();
  }
  if (offset > NodeLength(node)) {
    Throw(ctx, {"IndexSizeError", "The offset is larger than the node's length"});
    return qe::Undefined();
  }
  if (ComparePoints(node, offset, self->startNode, self->startOffset) < 0) return Signed(-1);
  if (ComparePoints(node, offset, self->endNode, self->endOffset) > 0) return Signed(1);
  return Signed(0);
}

Value IsPointInRange(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 2, "Range", "isPointInRange")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "isPointInRange");
  if (!node) return qe::Undefined();
  const uint32_t offset = qe::ToUint32(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (node->Root() != self->startNode->Root()) return qe::FromBool(false);
  if (node->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The node is a doctype"});
    return qe::Undefined();
  }
  if (offset > NodeLength(node)) {
    Throw(ctx, {"IndexSizeError", "The offset is larger than the node's length"});
    return qe::Undefined();
  }
  return qe::FromBool(!(ComparePoints(node, offset, self->startNode, self->startOffset) < 0 || ComparePoints(node, offset, self->endNode, self->endOffset) > 0));
}

Value IntersectsNode(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", "intersectsNode")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "intersectsNode");
  if (!node) return qe::Undefined();
  if (node->Root() != self->startNode->Root()) return qe::FromBool(false);
  Node* parent = node->parentNode;
  if (!parent) return qe::FromBool(true);
  const uint32_t offset = static_cast<uint32_t>(node->IndexInParent());
  return qe::FromBool(ComparePoints(parent, offset, self->endNode, self->endOffset) < 0 && ComparePoints(parent, offset + 1, self->startNode, self->startOffset) > 0);
}

Value DeleteContentsMethod(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  if (self) {
    if (auto error = DeleteContents(ctx, self)) Throw(ctx, *error);
  }
  return qe::Undefined();
}

Value ExtractContentsMethod(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self) return qe::Undefined();
  DocumentFragment* fragment = nullptr;
  if (auto error = ExtractContents(ctx, self, fragment)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(fragment);
}

Value CloneContentsMethod(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self) return qe::Undefined();
  DocumentFragment* fragment = nullptr;
  if (auto error = CloneContents(ctx, self, fragment)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(fragment);
}

Value InsertNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", "insertNode")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "insertNode");
  if (!node) return qe::Undefined();
  if (auto error = InsertNode(ctx, self, node)) Throw(ctx, *error);
  return qe::Undefined();
}

Value SurroundContentsMethod(Context& ctx, Value t, qe::Args args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self || Missing(ctx, args, 1, "Range", "surroundContents")) return qe::Undefined();
  Node* node = NodeAt(ctx, args, 0, "surroundContents");
  if (!node) return qe::Undefined();
  if (auto error = SurroundContents(ctx, self, node)) Throw(ctx, *error);
  return qe::Undefined();
}

Value CloneRange(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  if (!self) return qe::Undefined();
  Document* document = self->startNode->IsDocument() ? static_cast<Document*>(self->startNode) : self->startNode->nodeDocument;
  Range* copy = NewRange(ctx, document);
  copy->startNode = self->startNode;
  copy->startOffset = self->startOffset;
  copy->endNode = self->endNode;
  copy->endOffset = self->endOffset;
  return qe::FromObject(copy);
}

Value Detach(Context&, Value, qe::Args, Value) { return qe::Undefined(); }

Value ToStringMethod(Context& ctx, Value t, qe::Args, Value) {
  Range* self = LiveRange(ctx, t);
  return self ? qe::FromWtf8(ctx, RangeToString(self)) : qe::Undefined();
}

// ---- StaticRange ----

Value ConstructStaticRange(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'StaticRange': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (args.empty() || !qe::IsObject(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to construct 'StaticRange': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const Value init = args[0];
  Value startContainer = qe::Get(ctx, init, "startContainer");
  Value startOffset = qe::HasException(ctx) ? Value() : qe::Get(ctx, init, "startOffset");
  Value endContainer = qe::HasException(ctx) ? Value() : qe::Get(ctx, init, "endContainer");
  Value endOffset = qe::HasException(ctx) ? Value() : qe::Get(ctx, init, "endOffset");
  if (qe::HasException(ctx)) return qe::Undefined();
  Node* start = DOMObject::Cast<Node>(startContainer);
  Node* end = DOMObject::Cast<Node>(endContainer);
  if (!start || !end || qe::IsUndefined(startOffset) || qe::IsUndefined(endOffset)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'StaticRange': The dictionary is missing a required member.");
    return qe::Undefined();
  }
  const uint32_t startOffsetValue = qe::ToUint32(ctx, startOffset);
  const uint32_t endOffsetValue = qe::ToUint32(ctx, endOffset);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (start->nodeType == NodeType::Attribute || start->IsDocumentType() || end->nodeType == NodeType::Attribute || end->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The container is an attribute or a doctype"});
    return qe::Undefined();
  }
  Range* range = NewStaticRange(ctx);
  if (prototype) range->initialize_prototype(prototype);
  range->startNode = start;
  range->startOffset = startOffsetValue;
  range->endNode = end;
  range->endOffset = endOffsetValue;
  return qe::FromObject(range);
}

}  // namespace

Object* RangePrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_rangeKey)); }
Object* StaticRangePrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_staticRangeKey)); }

namespace {

Value CreateRange(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  return self ? qe::FromObject(NewRange(ctx, self)) : qe::Undefined();
}

}  // namespace

void DefineRangeClasses(Context& ctx) {
  qe::ClassRef abstract = qe::DefineClass(ctx, "AbstractRange", IllegalConstructor, 0);
  qe::DefineAccessor(abstract.prototype, "startContainer", GetStartContainer, nullptr);
  qe::DefineAccessor(abstract.prototype, "startOffset", GetStartOffset, nullptr);
  qe::DefineAccessor(abstract.prototype, "endContainer", GetEndContainer, nullptr);
  qe::DefineAccessor(abstract.prototype, "endOffset", GetEndOffset, nullptr);
  qe::DefineAccessor(abstract.prototype, "collapsed", GetCollapsed, nullptr);
  qe::DefineGlobal(ctx, "AbstractRange", abstract.constructor);

  qe::ClassRef range = qe::DefineClass(ctx, "Range", ConstructRange, 0, abstract.prototype);
  qe::SetRealmData(ctx, &g_rangeKey, range.prototype);
  Object* p = range.prototype;
  qe::DefineAccessor(p, "commonAncestorContainer", GetCommonAncestor, nullptr);
  qe::DefineMethod(p, "setStart", SetStart, 2);
  qe::DefineMethod(p, "setEnd", SetEnd, 2);
  qe::DefineMethod(p, "setStartBefore", SetNextTo<true, false>, 1);
  qe::DefineMethod(p, "setStartAfter", SetNextTo<true, true>, 1);
  qe::DefineMethod(p, "setEndBefore", SetNextTo<false, false>, 1);
  qe::DefineMethod(p, "setEndAfter", SetNextTo<false, true>, 1);
  qe::DefineMethod(p, "collapse", Collapse, 0);
  qe::DefineMethod(p, "selectNode", SelectNode, 1);
  qe::DefineMethod(p, "selectNodeContents", SelectNodeContents, 1);
  qe::DefineMethod(p, "compareBoundaryPoints", CompareBoundaryPoints, 2);
  qe::DefineMethod(p, "deleteContents", DeleteContentsMethod, 0);
  qe::DefineMethod(p, "extractContents", ExtractContentsMethod, 0);
  qe::DefineMethod(p, "cloneContents", CloneContentsMethod, 0);
  qe::DefineMethod(p, "insertNode", InsertNodeMethod, 1);
  qe::DefineMethod(p, "surroundContents", SurroundContentsMethod, 1);
  qe::DefineMethod(p, "cloneRange", CloneRange, 0);
  qe::DefineMethod(p, "detach", Detach, 0);
  qe::DefineMethod(p, "isPointInRange", IsPointInRange, 2);
  qe::DefineMethod(p, "comparePoint", ComparePointMethod, 2);
  qe::DefineMethod(p, "intersectsNode", IntersectsNode, 1);
  qe::DefineMethod(p, "toString", ToStringMethod, 0);
  qe::DefineGlobal(ctx, "Range", range.constructor);

  qe::ClassRef staticRange = qe::DefineClass(ctx, "StaticRange", ConstructStaticRange, 1, abstract.prototype);
  qe::SetRealmData(ctx, &g_staticRangeKey, staticRange.prototype);
  qe::DefineGlobal(ctx, "StaticRange", staticRange.constructor);

  if (Object* document = InterfacePrototype(ctx, Interface::Document)) qe::DefineMethod(document, "createRange", CreateRange, 0);
}

}  // namespace solar::dom
