#include <string>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/dom/Range.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

// The Selection of the Selection API: one range of its document, which stays where the content moves it.

namespace {

char g_selectionKey;

}  // namespace

struct JsSelection : DOMObject {
  Document* document = nullptr;
  Range* range = nullptr;
  bool backwards = false;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(document);
    visitor.Mark(range);
  }
};

namespace {

JsSelection* ThisSelection(Context& ctx, const Value& t) {
  JsSelection* selection = DOMObject::Cast<JsSelection>(t);
  if (!selection) qe::ThrowTypeError(ctx, "Illegal invocation");
  return selection;
}

bool Missing(Context& ctx, qe::Args args, size_t count, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'Selection': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") + " required, but only " +
                              std::to_string(args.size()) + " present.");
  return true;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

bool InDocument(const JsSelection* selection, Node* node) { return node->Root() == selection->document; }

void SetRange(JsSelection* selection, Range* range, bool backwards) {
  selection->range = range;
  selection->backwards = backwards;
  selection->NoteWrite();
}

Range* NewRangeAt(Context& ctx, JsSelection* selection, Node* startNode, uint32_t startOffset, Node* endNode, uint32_t endOffset) {
  Range* range = NewRange(ctx, selection->document);
  range->startNode = startNode;
  range->startOffset = startOffset;
  range->endNode = endNode;
  range->endOffset = endOffset;
  range->NoteWrite();
  return range;
}

// A point of the arguments the standard checks the same way everywhere: a doctype or a too large offset.
bool CheckPoint(Context& ctx, Node* node, uint32_t offset) {
  if (node->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The node is a doctype"});
    return false;
  }
  if (offset > NodeLength(node)) {
    Throw(ctx, {"IndexSizeError", "The offset " + std::to_string(offset) + " is larger than the node's length"});
    return false;
  }
  return true;
}

Value GetAnchorNode(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->range) return qe::Null();
  return NodeValue(self->backwards ? self->range->endNode : self->range->startNode);
}
Value GetAnchorOffset(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->range) return qe::FromUint32(0);
  return qe::FromUint32(self->backwards ? self->range->endOffset : self->range->startOffset);
}
Value GetFocusNode(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->range) return qe::Null();
  return NodeValue(self->backwards ? self->range->startNode : self->range->endNode);
}
Value GetFocusOffset(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->range) return qe::FromUint32(0);
  return qe::FromUint32(self->backwards ? self->range->startOffset : self->range->endOffset);
}
Value GetIsCollapsed(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  return self ? qe::FromBool(!self->range || self->range->Collapsed()) : qe::Undefined();
}
Value GetRangeCount(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  return self ? qe::FromUint32(self->range ? 1 : 0) : qe::Undefined();
}
Value GetType(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  return qe::FromWtf8(ctx, !self->range ? "None" : self->range->Collapsed() ? "Caret" : "Range");
}
Value GetDirection(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  return qe::FromWtf8(ctx, !self->range || self->range->Collapsed() ? "none" : self->backwards ? "backward" : "forward");
}

Value GetRangeAt(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "getRangeAt")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (index != 0 || !self->range) {
    Throw(ctx, {"IndexSizeError", "The index is out of range"});
    return qe::Undefined();
  }
  return qe::FromObject(self->range);
}

Value AddRange(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "addRange")) return qe::Undefined();
  Range* range = DOMObject::Cast<Range>(args[0]);
  if (!range || range->isStatic) {
    qe::ThrowTypeError(ctx, "Failed to execute 'addRange' on 'Selection': parameter 1 is not of type 'Range'.");
    return qe::Undefined();
  }
  if (!InDocument(self, range->startNode) || self->range) return qe::Undefined();
  SetRange(self, range, false);
  return qe::Undefined();
}

Value RemoveRange(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "removeRange")) return qe::Undefined();
  Range* range = DOMObject::Cast<Range>(args[0]);
  if (!range || range->isStatic) {
    qe::ThrowTypeError(ctx, "Failed to execute 'removeRange' on 'Selection': parameter 1 is not of type 'Range'.");
    return qe::Undefined();
  }
  if (range != self->range) {
    Throw(ctx, {"NotFoundError", "The given range is not in the selection"});
    return qe::Undefined();
  }
  SetRange(self, nullptr, false);
  return qe::Undefined();
}

Value RemoveAllRanges(Context& ctx, Value t, qe::Args, Value) {
  if (JsSelection* self = ThisSelection(ctx, t)) SetRange(self, nullptr, false);
  return qe::Undefined();
}

Value Collapse(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "collapse")) return qe::Undefined();
  if (qe::IsNull(args[0])) {
    SetRange(self, nullptr, false);
    return qe::Undefined();
  }
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'collapse' on 'Selection'");
  if (!node) return qe::Undefined();
  const uint32_t offset = args.size() > 1 ? qe::ToUint32(ctx, args[1]) : 0;
  if (qe::HasException(ctx) || !CheckPoint(ctx, node, offset)) return qe::Undefined();
  if (!InDocument(self, node)) return qe::Undefined();
  SetRange(self, NewRangeAt(ctx, self, node, offset, node, offset), false);
  return qe::Undefined();
}

template <bool ToStart>
Value CollapseToEdge(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->range) {
    Throw(ctx, {"InvalidStateError", "There is no selection to collapse"});
    return qe::Undefined();
  }
  Node* node = ToStart ? self->range->startNode : self->range->endNode;
  const uint32_t offset = ToStart ? self->range->startOffset : self->range->endOffset;
  SetRange(self, NewRangeAt(ctx, self, node, offset, node, offset), false);
  return qe::Undefined();
}

Value Extend(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "extend")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'extend' on 'Selection'");
  if (!node) return qe::Undefined();
  const uint32_t offset = args.size() > 1 ? qe::ToUint32(ctx, args[1]) : 0;
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!InDocument(self, node)) return qe::Undefined();
  if (!self->range) {
    Throw(ctx, {"InvalidStateError", "There is no selection to extend"});
    return qe::Undefined();
  }
  if (!CheckPoint(ctx, node, offset)) return qe::Undefined();
  Node* anchorNode = self->backwards ? self->range->endNode : self->range->startNode;
  const uint32_t anchorOffset = self->backwards ? self->range->endOffset : self->range->startOffset;
  Range* range;
  bool backwards = false;
  if (anchorNode->Root() != node->Root()) {
    range = NewRangeAt(ctx, self, node, offset, node, offset);
  } else if (ComparePoints(anchorNode, anchorOffset, node, offset) <= 0) {
    range = NewRangeAt(ctx, self, anchorNode, anchorOffset, node, offset);
  } else {
    range = NewRangeAt(ctx, self, node, offset, anchorNode, anchorOffset);
    backwards = true;
  }
  SetRange(self, range, backwards);
  return qe::Undefined();
}

Value SetBaseAndExtent(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 4, "setBaseAndExtent")) return qe::Undefined();
  Node* anchor = NodeArgument(ctx, args, 0, "Failed to execute 'setBaseAndExtent' on 'Selection'");
  if (!anchor) return qe::Undefined();
  const uint32_t anchorOffset = qe::ToUint32(ctx, args[1]);
  Node* focus = NodeArgument(ctx, args, 2, "Failed to execute 'setBaseAndExtent' on 'Selection'");
  if (!focus) return qe::Undefined();
  const uint32_t focusOffset = qe::ToUint32(ctx, args[3]);
  if (qe::HasException(ctx) || !CheckPoint(ctx, anchor, anchorOffset) || !CheckPoint(ctx, focus, focusOffset)) return qe::Undefined();
  if (!InDocument(self, anchor) || !InDocument(self, focus)) return qe::Undefined();
  if (ComparePoints(anchor, anchorOffset, focus, focusOffset) <= 0) SetRange(self, NewRangeAt(ctx, self, anchor, anchorOffset, focus, focusOffset), false);
  else SetRange(self, NewRangeAt(ctx, self, focus, focusOffset, anchor, anchorOffset), true);
  return qe::Undefined();
}

Value SelectAllChildren(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "selectAllChildren")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'selectAllChildren' on 'Selection'");
  if (!node) return qe::Undefined();
  if (node->IsDocumentType()) {
    Throw(ctx, {"InvalidNodeTypeError", "The node is a doctype"});
    return qe::Undefined();
  }
  if (!InDocument(self, node)) return qe::Undefined();
  SetRange(self, NewRangeAt(ctx, self, node, 0, node, NodeLength(node)), false);
  return qe::Undefined();
}

Value DeleteFromDocument(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (self && self->range) {
    if (auto error = DeleteContents(ctx, self->range)) Throw(ctx, *error);
  }
  return qe::Undefined();
}

Value ContainsNode(Context& ctx, Value t, qe::Args args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self || Missing(ctx, args, 1, "containsNode")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'containsNode' on 'Selection'");
  if (!node) return qe::Undefined();
  const bool partial = args.size() > 1 && args[1].to_boolean();
  if (!self->range || node->Root() != self->range->startNode->Root()) return qe::FromBool(false);
  const uint32_t length = NodeLength(node);
  if (partial) {
    return qe::FromBool(ComparePoints(node, 0, self->range->endNode, self->range->endOffset) < 0 && ComparePoints(node, length, self->range->startNode, self->range->startOffset) > 0);
  }
  return qe::FromBool(ComparePoints(node, 0, self->range->startNode, self->range->startOffset) >= 0 && ComparePoints(node, length, self->range->endNode, self->range->endOffset) <= 0);
}

Value ToStringMethod(Context& ctx, Value t, qe::Args, Value) {
  JsSelection* self = ThisSelection(ctx, t);
  if (!self) return qe::Undefined();
  return qe::FromWtf8(ctx, self->range ? RangeToString(self->range) : "");
}

Value GetSelection(Context& ctx, Value t, qe::Args, Value) {
  Document* self = ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  // Only a document with a window has one.
  if (!self->window) return qe::Null();
  if (!self->selection) {
    JsSelection* selection = Heap::Allocate<JsSelection>();
    selection->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_selectionKey)));
    selection->document = self;
    self->selection = selection;
    self->NoteWrite();
  }
  return qe::FromObject(self->selection);
}

}  // namespace

void DefineSelectionClass(Context& ctx) {
  qe::ClassRef selection = qe::DefineClass(ctx, "Selection", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_selectionKey, selection.prototype);
  Object* p = selection.prototype;
  qe::DefineAccessor(p, "anchorNode", GetAnchorNode, nullptr);
  qe::DefineAccessor(p, "anchorOffset", GetAnchorOffset, nullptr);
  qe::DefineAccessor(p, "focusNode", GetFocusNode, nullptr);
  qe::DefineAccessor(p, "focusOffset", GetFocusOffset, nullptr);
  qe::DefineAccessor(p, "isCollapsed", GetIsCollapsed, nullptr);
  qe::DefineAccessor(p, "rangeCount", GetRangeCount, nullptr);
  qe::DefineAccessor(p, "type", GetType, nullptr);
  qe::DefineAccessor(p, "direction", GetDirection, nullptr);
  qe::DefineMethod(p, "getRangeAt", GetRangeAt, 1);
  qe::DefineMethod(p, "addRange", AddRange, 1);
  qe::DefineMethod(p, "removeRange", RemoveRange, 1);
  qe::DefineMethod(p, "removeAllRanges", RemoveAllRanges, 0);
  qe::DefineMethod(p, "empty", RemoveAllRanges, 0);
  qe::DefineMethod(p, "collapse", Collapse, 1);
  qe::DefineMethod(p, "setPosition", Collapse, 1);
  qe::DefineMethod(p, "collapseToStart", CollapseToEdge<true>, 0);
  qe::DefineMethod(p, "collapseToEnd", CollapseToEdge<false>, 0);
  qe::DefineMethod(p, "extend", Extend, 1);
  qe::DefineMethod(p, "setBaseAndExtent", SetBaseAndExtent, 4);
  qe::DefineMethod(p, "selectAllChildren", SelectAllChildren, 1);
  qe::DefineMethod(p, "deleteFromDocument", DeleteFromDocument, 0);
  qe::DefineMethod(p, "containsNode", ContainsNode, 1);
  qe::DefineMethod(p, "toString", ToStringMethod, 0);
  qe::DefineGlobal(ctx, "Selection", selection.constructor);
  if (Object* document = InterfacePrototype(ctx, Interface::Document)) qe::DefineMethod(document, "getSelection", GetSelection, 0);
}

}  // namespace solar::dom
