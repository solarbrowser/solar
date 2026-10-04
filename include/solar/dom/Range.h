#pragma once

#include <optional>
#include <string>
#include <vector>

#include "solar/dom/Node.h"

// Ranges (https://dom.spec.whatwg.org/#ranges): a pair of boundary points in the tree, which stay where they
// are, as the content moves around them, when the range is live.
namespace solar::dom {

struct Range : Quanta::DOMObject {
  Node* startNode = nullptr;
  uint32_t startOffset = 0;
  Node* endNode = nullptr;
  uint32_t endOffset = 0;
  bool isStatic = false;  // a StaticRange: it keeps what it was made with

  Range() = default;
  Range(const Range&) = delete;
  ~Range();
  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(startNode);
    visitor.Mark(endNode);
  }

  bool Collapsed() const { return startNode == endNode && startOffset == endOffset; }
};

// The prototypes of Range and StaticRange in the realm.
Quanta::Object* RangePrototype(Quanta::Context& ctx);
Quanta::Object* StaticRangePrototype(Quanta::Context& ctx);
// A live range at the start of `document`, which is what `new Range()` and document.createRange() make.
Range* NewRange(Quanta::Context& ctx, Document* document);
Range* NewStaticRange(Quanta::Context& ctx);

// Where one boundary point is to another: -1 before, 0 equal, 1 after. The nodes must be in one tree.
int ComparePoints(const Node* nodeA, uint32_t offsetA, const Node* nodeB, uint32_t offsetB);

// "set the start or end": nullopt if all is well, or the error the standard raises.
std::optional<DomError> SetBoundary(Range* range, bool start, Node* node, uint32_t offset);

// The operations on the contents of a range.
std::optional<DomError> DeleteContents(Quanta::Context& ctx, Range* range);
std::optional<DomError> ExtractContents(Quanta::Context& ctx, Range* range, DocumentFragment*& result);
std::optional<DomError> CloneContents(Quanta::Context& ctx, Range* range, DocumentFragment*& result);
std::optional<DomError> InsertNode(Quanta::Context& ctx, Range* range, Node* node);
std::optional<DomError> SurroundContents(Quanta::Context& ctx, Range* range, Node* newParent);
std::string RangeToString(const Range* range);

// What the tree algorithms tell the live ranges, so that they follow the content.
bool HasLiveRanges();
void RangesBeforeRemove(Node* node, Node* parent, uint32_t index);
void RangesBeforeInsert(Node* parent, uint32_t index, uint32_t count);
void RangesReplaceData(Node* node, uint32_t offset, uint32_t count, uint32_t newLength);
void RangesSplitText(Node* node, Node* newNode, uint32_t offset);
// normalize() joining the Text node `joined` (a sibling after `node`) into `node`, which has `length` units before.
void RangesJoinText(Node* node, Node* joined, uint32_t length);

void DefineRangeClasses(Quanta::Context& ctx);
void DefineSelectionClass(Quanta::Context& ctx);

}  // namespace solar::dom
