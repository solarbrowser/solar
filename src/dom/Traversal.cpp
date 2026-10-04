#include "solar/dom/Traversal.h"

#include <algorithm>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

// NodeIterator, TreeWalker and NodeFilter (https://dom.spec.whatwg.org/#traversal).

namespace {

constexpr uint16_t kAccept = 1, kReject = 2, kSkip = 3;

char g_iteratorKey;
char g_walkerKey;

}  // namespace

struct Traversal : DOMObject {
  bool isIterator = false;
  Node* root = nullptr;
  uint32_t whatToShow = 0xFFFFFFFF;
  Value filter;
  bool active = false;
  // A TreeWalker's current node, or a NodeIterator's reference node.
  Node* current = nullptr;
  bool pointerBeforeReference = true;
  // While a NodeIterator traverses, where it is going to, which a removal moves as it does the reference.
  Node* candidate = nullptr;
  bool candidateBefore = true;
  bool hasCandidate = false;

  ~Traversal();
  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(root);
    visitor.Mark(filter);
    visitor.Mark(current);
    visitor.Mark(candidate);
  }
};

namespace {

std::vector<Traversal*>& Iterators() {
  static std::vector<Traversal*> iterators;
  return iterators;
}

}  // namespace

Traversal::~Traversal() {
  if (!isIterator) return;
  auto& list = Iterators();
  list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

bool HasNodeIterators() { return !Iterators().empty(); }

namespace {

// "adjusting a node pointer": where a pointer goes when `removed` is taken out of the tree.
void AdjustPointer(Node*& node, bool& before, const Traversal* iterator, Node* removed) {
  if (!removed->Contains(node) || removed->Contains(iterator->root)) return;
  if (before) {
    // The first node after the removed subtree that is still inside the root.
    for (Node* n = removed; n && n != iterator->root; n = n->parentNode) {
      if (n->nextSibling) {
        if (iterator->root->Contains(n->nextSibling)) {
          node = n->nextSibling;
          return;
        }
        break;
      }
    }
  }
  if (!removed->previousSibling) {
    node = removed->parentNode;
  } else {
    Node* last = removed->previousSibling;
    while (last->lastChild) last = last->lastChild;
    node = last;
  }
  before = false;
}

}  // namespace

void NodeIteratorsBeforeRemove(Node* toBeRemoved) {
  for (Traversal* iterator : Iterators()) {
    AdjustPointer(iterator->current, iterator->pointerBeforeReference, iterator, toBeRemoved);
    if (iterator->hasCandidate) AdjustPointer(iterator->candidate, iterator->candidateBefore, iterator, toBeRemoved);
    iterator->NoteWrite();
  }
}

namespace {

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Traversal* ThisTraversal(Context& ctx, const Value& t, bool iterator) {
  Traversal* self = DOMObject::Cast<Traversal>(t);
  if (!self || self->isIterator != iterator) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

// "filter": the result of the node filter for `node`, or false with an exception pending.
bool RunFilter(Context& ctx, Traversal* self, Node* node, uint16_t& result) {
  if (self->active) {
    Throw(ctx, {"InvalidStateError", "The filter is already running"});
    return false;
  }
  const uint32_t bit = static_cast<uint32_t>(node->nodeType) - 1;
  if (!(self->whatToShow & (1u << bit))) {
    result = kSkip;
    return true;
  }
  if (qe::IsNull(self->filter) || qe::IsUndefined(self->filter)) {
    result = kAccept;
    return true;
  }
  self->active = true;
  Value argument = qe::FromObject(node);
  Value value;
  if (qe::IsCallable(self->filter)) {
    value = qe::Call(ctx, self->filter, qe::Undefined(), qe::Args(&argument, 1));
  } else {
    Value method = qe::Get(ctx, self->filter, "acceptNode");
    if (qe::HasException(ctx)) {
      self->active = false;
      return false;
    }
    if (!qe::IsCallable(method)) {
      self->active = false;
      qe::ThrowTypeError(ctx, "The filter's acceptNode is not a function.");
      return false;
    }
    value = qe::Call(ctx, method, self->filter, qe::Args(&argument, 1));
  }
  self->active = false;
  if (qe::HasException(ctx)) return false;
  result = static_cast<uint16_t>(qe::ToUint32(ctx, value) & 0xFFFF);
  return !qe::HasException(ctx);
}

Value CreateTraversal(Context& ctx, Value t, qe::Args args, bool iterator) {
  Document* document = ThisDocument(ctx, t);
  if (!document) return qe::Undefined();
  const char* member = iterator ? "createNodeIterator" : "createTreeWalker";
  if (args.empty()) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'Document': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  Node* root = DOMObject::Cast<Node>(args[0]);
  if (!root) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'Document': parameter 1 is not of type 'Node'.");
    return qe::Undefined();
  }
  uint32_t whatToShow = 0xFFFFFFFF;
  if (args.size() > 1 && !qe::IsUndefined(args[1])) {
    whatToShow = qe::ToUint32(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  Value filter = qe::Null();
  if (args.size() > 2 && !qe::IsUndefined(args[2]) && !qe::IsNull(args[2])) {
    if (!qe::IsObject(args[2])) {
      qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'Document': parameter 3 is not of type 'NodeFilter'.");
      return qe::Undefined();
    }
    filter = args[2];
  }
  Traversal* traversal = Heap::Allocate<Traversal>();
  traversal->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, iterator ? &g_iteratorKey : &g_walkerKey)));
  traversal->isIterator = iterator;
  traversal->root = root;
  traversal->current = root;
  traversal->whatToShow = whatToShow;
  traversal->filter = filter;
  traversal->NoteWrite(filter);
  if (iterator) Iterators().push_back(traversal);
  return qe::FromObject(traversal);
}

Value CreateNodeIterator(Context& ctx, Value t, qe::Args args, Value) { return CreateTraversal(ctx, t, args, true); }
Value CreateTreeWalker(Context& ctx, Value t, qe::Args args, Value) { return CreateTraversal(ctx, t, args, false); }

// ---- Attributes shared by both ----

template <bool Iterator>
Value GetRoot(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, Iterator);
  return self ? qe::FromObject(self->root) : qe::Undefined();
}
template <bool Iterator>
Value GetWhatToShow(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, Iterator);
  return self ? qe::FromUint32(self->whatToShow) : qe::Undefined();
}
template <bool Iterator>
Value GetFilter(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, Iterator);
  return self ? (qe::IsUndefined(self->filter) ? qe::Null() : self->filter) : qe::Undefined();
}

Value GetReferenceNode(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, true);
  return self ? qe::FromObject(self->current) : qe::Undefined();
}
Value GetPointerBeforeReferenceNode(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, true);
  return self ? qe::FromBool(self->pointerBeforeReference) : qe::Undefined();
}
Value GetCurrentNode(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, false);
  return self ? qe::FromObject(self->current) : qe::Undefined();
}
Value SetCurrentNode(Context& ctx, Value t, qe::Args args, Value) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = args.empty() ? nullptr : DOMObject::Cast<Node>(args[0]);
  if (!node) {
    qe::ThrowTypeError(ctx, "Failed to set the 'currentNode' property on 'TreeWalker': The provided value is not of type 'Node'.");
    return qe::Undefined();
  }
  self->current = node;
  self->NoteWrite();
  return qe::Undefined();
}

// ---- NodeIterator ----

// The node after `node` in tree order within `root`, or null.
Node* FollowingIn(Node* node, Node* root) {
  if (node->firstChild) return node->firstChild;
  for (Node* n = node; n && n != root; n = n->parentNode) {
    if (n->nextSibling) return n->nextSibling;
  }
  return nullptr;
}

Node* PrecedingIn(Node* node, Node* root) {
  if (node == root) return nullptr;
  if (node->previousSibling) {
    Node* n = node->previousSibling;
    while (n->lastChild) n = n->lastChild;
    return n;
  }
  return node->parentNode;
}

Value IteratorTraverse(Context& ctx, Value t, bool next) {
  Traversal* self = ThisTraversal(ctx, t, true);
  if (!self) return qe::Undefined();
  self->candidate = self->current;
  self->candidateBefore = self->pointerBeforeReference;
  self->hasCandidate = true;
  Node* result = nullptr;
  for (;;) {
    if (next) {
      if (!self->candidateBefore) {
        Node* following = FollowingIn(self->candidate, self->root);
        if (!following) break;
        self->candidate = following;
        self->candidateBefore = false;
      } else {
        self->candidateBefore = false;
      }
    } else {
      if (self->candidateBefore) {
        Node* preceding = PrecedingIn(self->candidate, self->root);
        if (!preceding) break;
        self->candidate = preceding;
        self->candidateBefore = true;
      } else {
        self->candidateBefore = true;
      }
    }
    Node* node = self->candidate;
    uint16_t filterResult = 0;
    if (!RunFilter(ctx, self, node, filterResult)) {
      self->hasCandidate = false;
      self->candidate = nullptr;
      return qe::Undefined();
    }
    if (filterResult == kAccept) {
      self->current = self->candidate;
      self->pointerBeforeReference = self->candidateBefore;
      result = node;
      break;
    }
  }
  self->hasCandidate = false;
  self->candidate = nullptr;
  self->NoteWrite();
  return result ? qe::FromObject(result) : qe::Null();
}

Value IteratorNext(Context& ctx, Value t, qe::Args, Value) { return IteratorTraverse(ctx, t, true); }
Value IteratorPrevious(Context& ctx, Value t, qe::Args, Value) { return IteratorTraverse(ctx, t, false); }
Value Detach(Context&, Value, qe::Args, Value) { return qe::Undefined(); }

// ---- TreeWalker ----

Value WalkerParent(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = self->current;
  while (node && node != self->root) {
    node = node->parentNode;
    if (node) {
      uint16_t result = 0;
      if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
      if (result == kAccept) {
        self->current = node;
        self->NoteWrite();
        return qe::FromObject(node);
      }
    }
  }
  return qe::Null();
}

Value WalkerChild(Context& ctx, Value t, bool first) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = first ? self->current->firstChild : self->current->lastChild;
  while (node) {
    uint16_t result = 0;
    if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
    if (result == kAccept) {
      self->current = node;
      self->NoteWrite();
      return qe::FromObject(node);
    }
    if (result == kSkip) {
      Node* child = first ? node->firstChild : node->lastChild;
      if (child) {
        node = child;
        continue;
      }
    }
    while (node) {
      Node* sibling = first ? node->nextSibling : node->previousSibling;
      if (sibling) {
        node = sibling;
        break;
      }
      Node* parent = node->parentNode;
      if (!parent || parent == self->root || parent == self->current) return qe::Null();
      node = parent;
    }
  }
  return qe::Null();
}

Value WalkerFirstChild(Context& ctx, Value t, qe::Args, Value) { return WalkerChild(ctx, t, true); }
Value WalkerLastChild(Context& ctx, Value t, qe::Args, Value) { return WalkerChild(ctx, t, false); }

Value WalkerSibling(Context& ctx, Value t, bool next) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = self->current;
  if (node == self->root) return qe::Null();
  for (;;) {
    Node* sibling = next ? node->nextSibling : node->previousSibling;
    while (sibling) {
      node = sibling;
      uint16_t result = 0;
      if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
      if (result == kAccept) {
        self->current = node;
        self->NoteWrite();
        return qe::FromObject(node);
      }
      sibling = next ? node->firstChild : node->lastChild;
      if (result == kReject || !sibling) sibling = next ? node->nextSibling : node->previousSibling;
    }
    node = node->parentNode;
    if (!node || node == self->root) return qe::Null();
    uint16_t result = 0;
    if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
    if (result == kAccept) return qe::Null();
  }
}

Value WalkerNextSibling(Context& ctx, Value t, qe::Args, Value) { return WalkerSibling(ctx, t, true); }
Value WalkerPreviousSibling(Context& ctx, Value t, qe::Args, Value) { return WalkerSibling(ctx, t, false); }

Value WalkerPreviousNode(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = self->current;
  while (node != self->root) {
    Node* sibling = node->previousSibling;
    while (sibling) {
      node = sibling;
      uint16_t result = 0;
      if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
      while (result != kReject && node->lastChild) {
        node = node->lastChild;
        if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
      }
      if (result == kAccept) {
        self->current = node;
        self->NoteWrite();
        return qe::FromObject(node);
      }
      sibling = node->previousSibling;
    }
    if (node == self->root || !node->parentNode) return qe::Null();
    node = node->parentNode;
    uint16_t result = 0;
    if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
    if (result == kAccept) {
      self->current = node;
      self->NoteWrite();
      return qe::FromObject(node);
    }
  }
  return qe::Null();
}

Value WalkerNextNode(Context& ctx, Value t, qe::Args, Value) {
  Traversal* self = ThisTraversal(ctx, t, false);
  if (!self) return qe::Undefined();
  Node* node = self->current;
  uint16_t result = kAccept;
  for (;;) {
    while (result != kReject && node->firstChild) {
      node = node->firstChild;
      if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
      if (result == kAccept) {
        self->current = node;
        self->NoteWrite();
        return qe::FromObject(node);
      }
    }
    Node* sibling = nullptr;
    Node* temporary = node;
    while (temporary) {
      if (temporary == self->root) return qe::Null();
      sibling = temporary->nextSibling;
      if (sibling) {
        node = sibling;
        break;
      }
      temporary = temporary->parentNode;
    }
    if (!sibling) return qe::Null();
    if (!RunFilter(ctx, self, node, result)) return qe::Undefined();
    if (result == kAccept) {
      self->current = node;
      self->NoteWrite();
      return qe::FromObject(node);
    }
  }
}

}  // namespace

void DefineTraversalClasses(Context& ctx) {
  qe::ClassRef filter = qe::DefineClass(ctx, "NodeFilter", IllegalConstructor, 0);
  qe::DefineGlobal(ctx, "NodeFilter", filter.constructor);

  qe::ClassRef iterator = qe::DefineClass(ctx, "NodeIterator", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_iteratorKey, iterator.prototype);
  qe::DefineAccessor(iterator.prototype, "root", GetRoot<true>, nullptr);
  qe::DefineAccessor(iterator.prototype, "referenceNode", GetReferenceNode, nullptr);
  qe::DefineAccessor(iterator.prototype, "pointerBeforeReferenceNode", GetPointerBeforeReferenceNode, nullptr);
  qe::DefineAccessor(iterator.prototype, "whatToShow", GetWhatToShow<true>, nullptr);
  qe::DefineAccessor(iterator.prototype, "filter", GetFilter<true>, nullptr);
  qe::DefineMethod(iterator.prototype, "nextNode", IteratorNext, 0);
  qe::DefineMethod(iterator.prototype, "previousNode", IteratorPrevious, 0);
  qe::DefineMethod(iterator.prototype, "detach", Detach, 0);
  qe::DefineGlobal(ctx, "NodeIterator", iterator.constructor);

  qe::ClassRef walker = qe::DefineClass(ctx, "TreeWalker", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_walkerKey, walker.prototype);
  qe::DefineAccessor(walker.prototype, "root", GetRoot<false>, nullptr);
  qe::DefineAccessor(walker.prototype, "whatToShow", GetWhatToShow<false>, nullptr);
  qe::DefineAccessor(walker.prototype, "filter", GetFilter<false>, nullptr);
  qe::DefineAccessor(walker.prototype, "currentNode", GetCurrentNode, SetCurrentNode);
  qe::DefineMethod(walker.prototype, "parentNode", WalkerParent, 0);
  qe::DefineMethod(walker.prototype, "firstChild", WalkerFirstChild, 0);
  qe::DefineMethod(walker.prototype, "lastChild", WalkerLastChild, 0);
  qe::DefineMethod(walker.prototype, "previousSibling", WalkerPreviousSibling, 0);
  qe::DefineMethod(walker.prototype, "nextSibling", WalkerNextSibling, 0);
  qe::DefineMethod(walker.prototype, "previousNode", WalkerPreviousNode, 0);
  qe::DefineMethod(walker.prototype, "nextNode", WalkerNextNode, 0);
  qe::DefineGlobal(ctx, "TreeWalker", walker.constructor);

  if (Object* document = InterfacePrototype(ctx, Interface::Document)) {
    qe::DefineMethod(document, "createNodeIterator", CreateNodeIterator, 1);
    qe::DefineMethod(document, "createTreeWalker", CreateTreeWalker, 1);
  }
}

}  // namespace solar::dom
