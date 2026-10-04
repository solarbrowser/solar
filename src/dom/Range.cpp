#include "solar/dom/Range.h"

#include <algorithm>

namespace solar::dom {

namespace {

std::vector<Range*>& LiveRanges() {
  static std::vector<Range*> ranges;
  return ranges;
}

// The index of `node` among its parent's children.
uint32_t IndexOf(const Node* node) { return static_cast<uint32_t>(node->IndexInParent()); }


Node* RootOf(Node* node) { return node->Root(); }

}  // namespace

Range::~Range() {
  if (isStatic) return;
  auto& ranges = LiveRanges();
  ranges.erase(std::remove(ranges.begin(), ranges.end(), this), ranges.end());
}

Range* NewRange(Quanta::Context& ctx, Document* document) {
  Range* range = Quanta::Heap::Allocate<Range>();
  range->initialize_prototype(RangePrototype(ctx));
  range->startNode = range->endNode = document;
  LiveRanges().push_back(range);
  return range;
}

Range* NewStaticRange(Quanta::Context& ctx) {
  Range* range = Quanta::Heap::Allocate<Range>();
  range->initialize_prototype(StaticRangePrototype(ctx));
  range->isStatic = true;
  return range;
}

// ---- Boundary points ----

int ComparePoints(const Node* nodeA, uint32_t offsetA, const Node* nodeB, uint32_t offsetB) {
  if (nodeA == nodeB) return offsetA == offsetB ? 0 : offsetA < offsetB ? -1 : 1;
  // A node that follows another in tree order (a descendant does): compare the other way round.
  if (CompareDocumentPosition(const_cast<Node*>(nodeB), const_cast<Node*>(nodeA)) & kFollowing) {
    return ComparePoints(nodeB, offsetB, nodeA, offsetA) == 1 ? -1 : 1;
  }
  if (nodeA->Contains(nodeB)) {
    const Node* child = nodeB;
    while (child->parentNode != nodeA) child = child->parentNode;
    if (IndexOf(child) < offsetA) return 1;
  }
  return -1;
}

std::optional<DomError> SetBoundary(Range* range, bool start, Node* node, uint32_t offset) {
  if (node->IsDocumentType()) return DomError{"InvalidNodeTypeError", "The node provided is a doctype, which cannot hold a boundary point"};
  if (offset > NodeLength(node)) return DomError{"IndexSizeError", "The offset " + std::to_string(offset) + " is larger than the node's length (" + std::to_string(NodeLength(node)) + ")."};
  if (range->isStatic) {
    (start ? range->startNode : range->endNode) = node;
    (start ? range->startOffset : range->endOffset) = offset;
    return std::nullopt;
  }
  if (start) {
    // A start after the end, or in another tree, pulls the end to it.
    if (RootOf(range->startNode) != RootOf(node) || ComparePoints(node, offset, range->endNode, range->endOffset) > 0) {
      range->endNode = node;
      range->endOffset = offset;
    }
    range->startNode = node;
    range->startOffset = offset;
  } else {
    if (RootOf(range->startNode) != RootOf(node) || ComparePoints(node, offset, range->startNode, range->startOffset) < 0) {
      range->startNode = node;
      range->startOffset = offset;
    }
    range->endNode = node;
    range->endOffset = offset;
  }
  range->NoteWrite();
  return std::nullopt;
}

// ---- What the tree tells the live ranges ----

bool HasLiveRanges() { return !LiveRanges().empty(); }

void RangesBeforeRemove(Node* node, Node* parent, uint32_t index) {
  for (Range* range : LiveRanges()) {
    if (node->Contains(range->startNode)) {
      range->startNode = parent;
      range->startOffset = index;
    }
    if (node->Contains(range->endNode)) {
      range->endNode = parent;
      range->endOffset = index;
    }
    if (range->startNode == parent && range->startOffset > index) --range->startOffset;
    if (range->endNode == parent && range->endOffset > index) --range->endOffset;
    range->NoteWrite();
  }
}

void RangesBeforeInsert(Node* parent, uint32_t index, uint32_t count) {
  for (Range* range : LiveRanges()) {
    if (range->startNode == parent && range->startOffset > index) range->startOffset += count;
    if (range->endNode == parent && range->endOffset > index) range->endOffset += count;
  }
}

void RangesReplaceData(Node* node, uint32_t offset, uint32_t count, uint32_t newLength) {
  for (Range* range : LiveRanges()) {
    if (range->startNode == node) {
      if (range->startOffset > offset && range->startOffset <= offset + count) range->startOffset = offset;
      else if (range->startOffset > offset + count) range->startOffset = range->startOffset + newLength - count;
    }
    if (range->endNode == node) {
      if (range->endOffset > offset && range->endOffset <= offset + count) range->endOffset = offset;
      else if (range->endOffset > offset + count) range->endOffset = range->endOffset + newLength - count;
    }
  }
}

void RangesSplitText(Node* node, Node* newNode, uint32_t offset) {
  Node* parent = node->parentNode;
  const uint32_t index = IndexOf(node);
  for (Range* range : LiveRanges()) {
    if (range->startNode == node && range->startOffset > offset) {
      range->startNode = newNode;
      range->startOffset -= offset;
    }
    if (range->endNode == node && range->endOffset > offset) {
      range->endNode = newNode;
      range->endOffset -= offset;
    }
    if (parent && range->startNode == parent && range->startOffset == index + 1) ++range->startOffset;
    if (parent && range->endNode == parent && range->endOffset == index + 1) ++range->endOffset;
    range->NoteWrite();
  }
}

void RangesJoinText(Node* node, Node* joined, uint32_t length) {
  Node* parent = joined->parentNode;
  const uint32_t index = IndexOf(joined);
  for (Range* range : LiveRanges()) {
    if (range->startNode == joined) {
      range->startNode = node;
      range->startOffset += length;
    } else if (parent && range->startNode == parent && range->startOffset == index) {
      range->startNode = node;
      range->startOffset = length;
    }
    if (range->endNode == joined) {
      range->endNode = node;
      range->endOffset += length;
    } else if (parent && range->endNode == parent && range->endOffset == index) {
      range->endNode = node;
      range->endOffset = length;
    }
    range->NoteWrite();
  }
}

// ---- The contents ----

namespace {

// A node is contained in a range when it is in the same tree and wholly between its boundary points.
bool IsContained(const Node* node, const Range* range) {
  if (RootOf(const_cast<Node*>(node)) != RootOf(range->startNode)) return false;
  return ComparePoints(node, 0, range->startNode, range->startOffset) > 0 && ComparePoints(node, NodeLength(node), range->endNode, range->endOffset) < 0;
}

// Partially contained: an inclusive ancestor of exactly one of the two boundary nodes.
bool IsPartiallyContained(const Node* node, const Range* range) {
  const bool startInside = node->Contains(range->startNode);
  const bool endInside = node->Contains(range->endNode);
  return startInside != endInside;
}

std::string Substring(const CharacterData* node, uint32_t offset, uint32_t count) {
  const std::u16string units = ToUtf16(node->data);
  return FromUtf16(std::u16string_view(units).substr(offset, count));
}

struct Parts {
  Node* commonAncestor = nullptr;
  Node* firstPartial = nullptr;
  Node* lastPartial = nullptr;
  std::vector<Node*> contained;
};

std::optional<DomError> Analyze(const Range* range, Parts& parts) {
  Node* common = range->startNode;
  while (!common->Contains(range->endNode)) common = common->parentNode;
  parts.commonAncestor = common;
  if (!range->startNode->Contains(range->endNode)) {
    for (Node* child = common->firstChild; child; child = child->nextSibling) {
      if (IsPartiallyContained(child, range)) {
        parts.firstPartial = child;
        break;
      }
    }
  }
  if (!range->endNode->Contains(range->startNode)) {
    for (Node* child = common->lastChild; child; child = child->previousSibling) {
      if (IsPartiallyContained(child, range)) {
        parts.lastPartial = child;
        break;
      }
    }
  }
  for (Node* child = common->firstChild; child; child = child->nextSibling) {
    if (IsContained(child, range)) parts.contained.push_back(child);
  }
  for (const Node* node : parts.contained) {
    if (node->IsDocumentType()) return DomError{"HierarchyRequestError", "The range contains a doctype"};
  }
  return std::nullopt;
}

// Where a range ends up after its contents are taken: collapsed at the node it was started from, or just after it.
void CollapseAfter(Range* range, Node*& newNode, uint32_t& newOffset) {
  if (range->startNode->Contains(range->endNode)) {
    newNode = range->startNode;
    newOffset = range->startOffset;
    return;
  }
  Node* reference = range->startNode;
  while (!reference->parentNode->Contains(range->endNode)) reference = reference->parentNode;
  newNode = reference->parentNode;
  newOffset = IndexOf(reference) + 1;
}

std::optional<DomError> CloneOrExtract(Quanta::Context& ctx, Range* range, bool extract, DocumentFragment*& result) {
  Document* document = range->startNode->IsDocument() ? static_cast<Document*>(range->startNode) : range->startNode->nodeDocument;
  DocumentFragment* fragment = NewDocumentFragment(ctx, document);
  result = fragment;
  if (range->Collapsed()) return std::nullopt;

  Node* const startNode = range->startNode;
  Node* const endNode = range->endNode;
  const uint32_t startOffset = range->startOffset;
  const uint32_t endOffset = range->endOffset;

  if (startNode == endNode && startNode->IsCharacterData()) {
    CharacterData* original = static_cast<CharacterData*>(startNode);
    CharacterData* clone = static_cast<CharacterData*>(CloneNode(ctx, original, false));
    clone->data = Substring(original, startOffset, endOffset - startOffset);
    InsertUnchecked(clone, fragment, nullptr);
    if (extract) ReplaceData(original, startOffset, endOffset - startOffset, "");
    return std::nullopt;
  }

  Parts parts;
  if (auto error = Analyze(range, parts)) return error;

  Node* newNode = nullptr;
  uint32_t newOffset = 0;
  if (extract) CollapseAfter(range, newNode, newOffset);

  const auto subrange = [&](Node* sNode, uint32_t sOffset, Node* eNode, uint32_t eOffset) {
    Range* sub = NewRange(ctx, document);
    sub->startNode = sNode;
    sub->startOffset = sOffset;
    sub->endNode = eNode;
    sub->endOffset = eOffset;
    DocumentFragment* part = nullptr;
    CloneOrExtract(ctx, sub, extract, part);
    return part;
  };

  // The start: the start node itself if it is character data and not an ancestor of the end, else a partly contained child.
  if (!startNode->Contains(endNode)) {
    Node* first = parts.firstPartial;
    if (first && first == startNode && startNode->IsCharacterData()) {
      CharacterData* original = static_cast<CharacterData*>(startNode);
      CharacterData* clone = static_cast<CharacterData*>(CloneNode(ctx, original, false));
      clone->data = Substring(original, startOffset, NodeLength(original) - startOffset);
      InsertUnchecked(clone, fragment, nullptr);
      if (extract) ReplaceData(original, startOffset, NodeLength(original) - startOffset, "");
    } else if (first) {
      Node* clone = CloneNode(ctx, first, false);
      InsertUnchecked(clone, fragment, nullptr);
      DocumentFragment* part = subrange(startNode, startOffset, first, NodeLength(first));
      InsertUnchecked(part, clone, nullptr);
    }
  }
  for (Node* child : parts.contained) {
    if (extract) InsertUnchecked(child, fragment, nullptr);
    else InsertUnchecked(CloneNode(ctx, child, true), fragment, nullptr);
  }
  if (!endNode->Contains(startNode)) {
    Node* last = parts.lastPartial;
    if (last && last == endNode && endNode->IsCharacterData()) {
      CharacterData* original = static_cast<CharacterData*>(endNode);
      CharacterData* clone = static_cast<CharacterData*>(CloneNode(ctx, original, false));
      clone->data = Substring(original, 0, endOffset);
      InsertUnchecked(clone, fragment, nullptr);
      if (extract) ReplaceData(original, 0, endOffset, "");
    } else if (last) {
      Node* clone = CloneNode(ctx, last, false);
      InsertUnchecked(clone, fragment, nullptr);
      DocumentFragment* part = subrange(last, 0, endNode, endOffset);
      InsertUnchecked(part, clone, nullptr);
    }
  }
  if (extract) {
    range->startNode = range->endNode = newNode;
    range->startOffset = range->endOffset = newOffset;
    range->NoteWrite();
  }
  return std::nullopt;
}

}  // namespace

std::optional<DomError> CloneContents(Quanta::Context& ctx, Range* range, DocumentFragment*& result) { return CloneOrExtract(ctx, range, false, result); }
std::optional<DomError> ExtractContents(Quanta::Context& ctx, Range* range, DocumentFragment*& result) { return CloneOrExtract(ctx, range, true, result); }

std::optional<DomError> DeleteContents(Quanta::Context& ctx, Range* range) {
  (void)ctx;
  if (range->Collapsed()) return std::nullopt;
  Node* const startNode = range->startNode;
  Node* const endNode = range->endNode;
  const uint32_t startOffset = range->startOffset;
  const uint32_t endOffset = range->endOffset;
  if (startNode == endNode && startNode->IsCharacterData()) {
    ReplaceData(static_cast<CharacterData*>(startNode), startOffset, endOffset - startOffset, "");
    return std::nullopt;
  }
  // The nodes to remove: contained ones whose parents are not.
  std::vector<Node*> toRemove;
  Node* root = startNode->Root();
  for (Node* node = root; node; node = node->NextInTree(root)) {
    if (node != root && IsContained(node, range) && !(node->parentNode && IsContained(node->parentNode, range))) toRemove.push_back(node);
  }
  Node* newNode = nullptr;
  uint32_t newOffset = 0;
  CollapseAfter(range, newNode, newOffset);
  if (startNode->IsCharacterData()) ReplaceData(static_cast<CharacterData*>(startNode), startOffset, NodeLength(startNode) - startOffset, "");
  for (Node* node : toRemove) RemoveUnchecked(node);
  if (endNode->IsCharacterData()) ReplaceData(static_cast<CharacterData*>(endNode), 0, endOffset, "");
  range->startNode = range->endNode = newNode;
  range->startOffset = range->endOffset = newOffset;
  range->NoteWrite();
  return std::nullopt;
}

std::optional<DomError> InsertNode(Quanta::Context& ctx, Range* range, Node* node) {
  Node* start = range->startNode;
  if (start->nodeType == NodeType::ProcessingInstruction || start->nodeType == NodeType::Comment || (start->IsText() && !start->parentNode) || node == start) {
    return DomError{"HierarchyRequestError", "The range's start cannot take a node"};
  }
  Node* reference = nullptr;
  if (start->IsText()) {
    reference = start;
  } else {
    uint32_t i = 0;
    for (Node* child = start->firstChild; child; child = child->nextSibling, ++i) {
      if (i == range->startOffset) {
        reference = child;
        break;
      }
    }
  }
  Node* parent = reference ? reference->parentNode : start;
  if (auto error = EnsurePreInsertionValidity(node, parent, reference)) return error;
  if (start->IsText()) {
    // Split the text: the new node, after the first part, is what the insertion goes before.
    CharacterData* text = static_cast<CharacterData*>(start);
    const std::u16string units = ToUtf16(text->data);
    const uint32_t offset = range->startOffset;
    CharacterData* tail = NewText(ctx, text->nodeDocument, FromUtf16(std::u16string_view(units).substr(offset)));
    InsertUnchecked(tail, text->parentNode, text->nextSibling);
    RangesSplitText(text, tail, offset);
    ReplaceData(text, offset, static_cast<uint32_t>(units.size()) - offset, "");
    reference = tail;
  }
  if (node == reference) reference = node->nextSibling;
  if (node->parentNode) RemoveUnchecked(node);
  uint32_t newOffset = reference ? IndexOf(reference) : NodeLength(parent);
  newOffset += node->IsFragment() ? NodeLength(node) : 1;
  if (auto error = PreInsert(node, parent, reference)) return error;
  if (range->Collapsed()) {
    range->endNode = parent;
    range->endOffset = newOffset;
    range->NoteWrite();
  }
  return std::nullopt;
}

std::optional<DomError> SurroundContents(Quanta::Context& ctx, Range* range, Node* newParent) {
  // A non-Text node that is only partly contained cannot be wrapped.
  for (Node* node = range->startNode->Root(); node; node = node->NextInTree(range->startNode->Root())) {
    if (!node->IsText() && IsPartiallyContained(node, range)) return DomError{"InvalidStateError", "The range partially contains a non-Text node"};
  }
  if (newParent->IsDocument() || newParent->IsDocumentType() || newParent->IsFragment()) return DomError{"InvalidNodeTypeError", "The node cannot surround a range"};
  DocumentFragment* fragment = nullptr;
  if (auto error = ExtractContents(ctx, range, fragment)) return error;
  if (newParent->firstChild) ReplaceAll(newParent, nullptr);
  if (auto error = InsertNode(ctx, range, newParent)) return error;
  if (auto error = AppendChild(newParent, fragment)) return error;
  range->startNode = range->endNode = newParent->parentNode;
  range->startOffset = IndexOf(newParent);
  range->endOffset = range->startOffset + 1;
  range->NoteWrite();
  return std::nullopt;
}

std::string RangeToString(const Range* range) {
  std::string out;
  if (range->startNode == range->endNode && range->startNode->IsText()) return Substring(static_cast<const CharacterData*>(range->startNode), range->startOffset, range->endOffset - range->startOffset);
  if (range->startNode->IsText()) out += Substring(static_cast<const CharacterData*>(range->startNode), range->startOffset, NodeLength(range->startNode) - range->startOffset);
  Node* common = range->startNode;
  while (!common->Contains(range->endNode)) common = common->parentNode;
  for (Node* node = common; node; node = node->NextInTree(common)) {
    if (node->IsText() && node != range->startNode && node != range->endNode && IsContained(node, range)) out += static_cast<CharacterData*>(node)->data;
  }
  if (range->endNode->IsText() && range->endNode != range->startNode) out += Substring(static_cast<const CharacterData*>(range->endNode), 0, range->endOffset);
  return out;
}

}  // namespace solar::dom
