#include <algorithm>

#include "solar/dom/Mutation.h"
#include "solar/dom/Node.h"

namespace solar::dom {

// Shadow trees (https://dom.spec.whatwg.org/#shadow-trees): attaching, slots and what they are given, and
// what an event's path asks a node about the tree it is in.

namespace {

size_t g_shadowRoots = 0;

ShadowRoot* AsShadowRoot(Node* node) {
  return node && node->IsFragment() && static_cast<DocumentFragment*>(node)->isShadowRoot ? static_cast<ShadowRoot*>(node) : nullptr;
}

bool Contains(const std::vector<Node*>& list, const Node* node) { return std::find(list.begin(), list.end(), node) != list.end(); }

// The slots below `root`, in tree order.
std::vector<Element*> SlotsIn(Node* root) {
  std::vector<Element*> slots;
  for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
    if (IsSlot(node)) slots.push_back(static_cast<Element*>(node));
  }
  return slots;
}

// "find slottables".
std::vector<Node*> FindSlottables(Element* slot) {
  std::vector<Node*> result;
  ShadowRoot* root = AsShadowRoot(slot->Root());
  if (!root) return result;
  Element* host = root->host;
  if (root->slotAssignment == SlotAssignment::Manual) {
    for (Node* slottable : slot->manuallyAssignedNodes) {
      if (slottable->parentNode == host) result.push_back(slottable);
    }
  } else {
    for (Node* child = host->firstChild; child; child = child->nextSibling) {
      if (IsSlottable(child) && FindSlot(child, false) == slot) result.push_back(child);
    }
  }
  return result;
}

void AssignSlot(Node* slottable) {
  if (Element* slot = FindSlot(slottable, false)) AssignSlottablesForSlot(slot);
}

bool Equivalent(const std::optional<std::string>& a, const std::optional<std::string>& b) {
  // A missing value and an empty one name the same slot.
  return a.value_or("") == b.value_or("");
}

}  // namespace

bool IsValidCustomElementName(std::string_view name) {
  if (name.empty() || name[0] < 'a' || name[0] > 'z' || name.find('-') == std::string_view::npos) return false;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') return false;
  }
  static const std::string_view kReserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri", "font-face-format", "font-face-name", "missing-glyph"};
  return std::find(std::begin(kReserved), std::end(kReserved), name) == std::end(kReserved);
}

// ---- Trees ----

bool IsShadowIncludingInclusiveAncestor(const Node* a, const Node* b) {
  for (const Node* node = b; node;) {
    if (node == a) return true;
    if (node->parentNode) {
      node = node->parentNode;
    } else if (node->IsFragment() && static_cast<const DocumentFragment*>(node)->isShadowRoot) {
      node = static_cast<const DocumentFragment*>(node)->host;
    } else {
      return false;
    }
  }
  return false;
}

Node* ShadowIncludingRoot(Node* node) {
  Node* root = node->Root();
  while (ShadowRoot* shadow = AsShadowRoot(root)) root = shadow->host->Root();
  return root;
}

Node* Retarget(Node* a, Node* against) {
  for (;;) {
    ShadowRoot* shadow = AsShadowRoot(a->Root());
    if (!shadow) return a;
    if (against && IsShadowIncludingInclusiveAncestor(shadow, against)) return a;
    a = shadow->host;
  }
}

ShadowRoot* ContainingShadowRoot(Node* node) { return AsShadowRoot(node->Root()); }

// ---- Attaching ----

std::optional<DomError> AttachShadow(Quanta::Context& ctx, Element* element, ShadowMode mode, SlotAssignment slotAssignment, bool delegatesFocus, bool clonable, bool serializable,
                                     ShadowRoot*& out) {
  if (element->namespaceUri != kHtmlNamespace) return DomError{"NotSupportedError", "This element does not support attachShadow"};
  static const std::string_view kHosts[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span"};
  if (!IsValidCustomElementName(element->localName) && std::find(std::begin(kHosts), std::end(kHosts), element->localName) == std::end(kHosts)) {
    return DomError{"NotSupportedError", "This element does not support attachShadow"};
  }
  if (ShadowRoot* existing = element->shadowRoot) {
    if (!existing->declarative) return DomError{"NotSupportedError", "Shadow root cannot be created on a host which already hosts a shadow tree."};
    // A declarative shadow root is one a script may take over, if it has the mode the parser gave it: its children go and its other settings stay.
    if (existing->mode != mode) {
      return DomError{"NotSupportedError", "Shadow root cannot be created on a host which already hosts a shadow tree."};
    }
    while (existing->firstChild) RemoveUnchecked(existing->firstChild);
    existing->declarative = false;
    out = existing;
    return std::nullopt;
  }
  ShadowRoot* shadow = Quanta::Heap::Allocate<ShadowRoot>();
  shadow->nodeType = NodeType::DocumentFragment;
  shadow->nodeDocument = element->nodeDocument;
  shadow->ops = NodeEventOps();
  shadow->initialize_prototype(InterfacePrototype(ctx, Interface::ShadowRoot));
  shadow->isShadowRoot = true;
  shadow->host = element;
  shadow->mode = mode;
  shadow->slotAssignment = slotAssignment;
  shadow->delegatesFocus = delegatesFocus;
  shadow->clonable = clonable;
  shadow->serializable = serializable;
  element->shadowRoot = shadow;
  element->NoteWrite();
  ++g_shadowRoots;
  out = shadow;
  return std::nullopt;
}

// ---- Slots ----

bool IsSlot(const Node* node) {
  const Element* element = AsElement(node);
  return element && element->IsHtml("slot");
}

bool IsSlottable(const Node* node) { return node->IsElement() || node->IsText(); }

std::string SlotName(const Element* slot) {
  const Attr* name = slot->FindAttribute("", "name");
  return name ? name->value : "";
}

Element* FindSlot(Node* slottable, bool open) {
  Element* parent = AsElement(slottable->parentNode);
  if (!parent) return nullptr;
  ShadowRoot* shadow = parent->shadowRoot;
  if (!shadow) return nullptr;
  if (open && shadow->mode != ShadowMode::Open) return nullptr;
  if (shadow->slotAssignment == SlotAssignment::Manual) {
    for (Element* slot : SlotsIn(shadow)) {
      if (Contains(slot->manuallyAssignedNodes, slottable)) return slot;
    }
    return nullptr;
  }
  std::string name;
  if (const Element* element = AsElement(slottable)) {
    if (const Attr* attribute = element->FindAttribute("", "slot")) name = attribute->value;
  }
  for (Element* slot : SlotsIn(shadow)) {
    if (SlotName(slot) == name) return slot;
  }
  return nullptr;
}

std::vector<Node*> FindFlattenedSlottables(Element* slot) {
  std::vector<Node*> result;
  if (!AsShadowRoot(slot->Root())) return result;
  std::vector<Node*> slottables = FindSlottables(slot);
  if (slottables.empty()) {
    for (Node* child = slot->firstChild; child; child = child->nextSibling) {
      if (IsSlottable(child)) slottables.push_back(child);
    }
  }
  for (Node* node : slottables) {
    if (IsSlot(node) && AsShadowRoot(node->Root())) {
      for (Node* inner : FindFlattenedSlottables(static_cast<Element*>(node))) result.push_back(inner);
    } else {
      result.push_back(node);
    }
  }
  return result;
}

void AssignSlottablesForSlot(Element* slot) {
  std::vector<Node*> slottables = FindSlottables(slot);
  if (slottables != slot->assignedNodes) QueueSlotChange(slot);
  for (Node* old : slot->assignedNodes) {
    if (old->assignedSlot == slot && !Contains(slottables, old)) {
      old->assignedSlot = nullptr;
      old->NoteWrite();
    }
  }
  slot->assignedNodes = slottables;
  for (Node* slottable : slottables) {
    slottable->assignedSlot = slot;
    slottable->NoteWrite();
  }
  slot->NoteWrite();
}

void AssignSlottablesForTree(Node* root) {
  if (IsSlot(root)) AssignSlottablesForSlot(static_cast<Element*>(root));
  for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
    if (IsSlot(node)) AssignSlottablesForSlot(static_cast<Element*>(node));
  }
}

// ---- What the tree algorithms tell the shadow trees ----

bool HasShadowTrees() { return g_shadowRoots > 0; }

void ShadowAfterInsert(Node* node, Node* parent) {
  Element* host = AsElement(parent);
  if (host && host->shadowRoot && IsSlottable(node)) AssignSlot(node);
  if (AsShadowRoot(parent->Root()) && IsSlot(parent) && static_cast<Element*>(parent)->assignedNodes.empty()) QueueSlotChange(static_cast<Element*>(parent));
  AssignSlottablesForTree(node->Root());
}

void ShadowAfterRemove(Node* node, Node* parent, Element* wasAssignedTo) {
  if (wasAssignedTo) AssignSlottablesForSlot(wasAssignedTo);
  if (AsShadowRoot(parent->Root()) && IsSlot(parent) && static_cast<Element*>(parent)->assignedNodes.empty()) QueueSlotChange(static_cast<Element*>(parent));
  bool hasSlot = IsSlot(node);
  for (Node* n = node->NextInTree(node); n && !hasSlot; n = n->NextInTree(node)) hasSlot = IsSlot(n);
  if (hasSlot) {
    AssignSlottablesForTree(parent->Root());
    AssignSlottablesForTree(node);
  }
}

void ShadowAttributeChanged(Element* element, const std::string& name, const std::string& namespaceUri, const std::optional<std::string>& oldValue,
                            const std::optional<std::string>& newValue) {
  if (!namespaceUri.empty()) return;
  if (name == "slot") {
    if (Equivalent(oldValue, newValue)) return;
    if (element->assignedSlot) AssignSlottablesForSlot(element->assignedSlot);
    AssignSlot(element);
  } else if (name == "name" && IsSlot(element)) {
    if (Equivalent(oldValue, newValue)) return;
    AssignSlottablesForTree(element->Root());
  }
}

// ---- What dispatch asks ----

namespace {

web::JsEventTarget* GetParent(web::JsEventTarget* self, web::JsEvent* event) {
  Node* node = static_cast<Node*>(self);
  if (node->IsDocument()) return event->type == "load" ? nullptr : static_cast<Document*>(node)->window;
  if (ShadowRoot* shadow = AsShadowRoot(node)) {
    // Unless the event is composed, a shadow root is as far as it goes from inside it.
    if (!event->composed && !event->eventPath.empty() && static_cast<Node*>(event->eventPath[0].invocationTarget)->Root() == shadow) return nullptr;
    return shadow->host;
  }
  return node->assignedSlot ? static_cast<web::JsEventTarget*>(node->assignedSlot) : static_cast<web::JsEventTarget*>(node->parentNode);
}

web::JsEventTarget* RetargetOps(web::JsEventTarget* a, web::JsEventTarget* against) {
  // What an event is retargeted against can be the window, which is no node and in no tree: then it is nothing any shadow tree is in.
  return Retarget(static_cast<Node*>(a), against && against->ops ? static_cast<Node*>(against) : nullptr);
}

bool RootIncludes(web::JsEventTarget* a, web::JsEventTarget* b) { return IsShadowIncludingInclusiveAncestor(static_cast<Node*>(a)->Root(), static_cast<Node*>(b)); }
bool RootIsShadowRoot(web::JsEventTarget* a) { return AsShadowRoot(static_cast<Node*>(a)->Root()) != nullptr; }
bool RootIsClosedShadowRoot(web::JsEventTarget* a) {
  ShadowRoot* shadow = AsShadowRoot(static_cast<Node*>(a)->Root());
  return shadow && shadow->mode == ShadowMode::Closed;
}
bool IsClosedShadowRoot(web::JsEventTarget* a) {
  ShadowRoot* shadow = AsShadowRoot(static_cast<Node*>(a));
  return shadow && shadow->mode == ShadowMode::Closed;
}
bool IsAssigned(web::JsEventTarget* a) { return static_cast<Node*>(a)->assignedSlot != nullptr; }

const web::EventTargetOps kNodeOps = {GetParent, RetargetOps, RootIncludes, RootIsShadowRoot, RootIsClosedShadowRoot, IsClosedShadowRoot, IsAssigned};

}  // namespace

const web::EventTargetOps* NodeEventOps() { return &kNodeOps; }

}  // namespace solar::dom
