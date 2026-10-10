#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "solar/dom/Node.h"

// What script has changed in a document since the page was last put together, kept so that the work after a change is only for what
// changed. Changes go to a pending set as they happen (a node changed a hundred times is one entry, its flags the union); the layout
// and painting steps work from the committed set, which they take whole when they are ready for it and which script never touches.
// Script goes on writing into pending while a committed set is being worked on; what it writes then is committed after.
namespace solar::dom {

enum DamageFlag : uint8_t {
  kDamageAttributes = 1,  // an attribute changed: a style may have changed with it
  kDamageChildren = 2,    // children were added or removed
  kDamageText = 4,        // character data changed
  kDamageStyleSheets = 8, // the rules of the document changed: any element's style may have
};

struct DamageEntry {
  Node* node;
  uint8_t flags;
};

class DamageTracker {
 public:
  // Nothing is recorded until something wants it.
  void Enable() { enabled_ = true; }
  bool Enabled() const { return enabled_; }

  // A change to the node. Cheap, and the same node again only adds flags.
  void Note(Node* node, uint8_t flags);
  // The node and everything under it are going away or leaving the tree: none of them is to be worked on.
  void ForgetSubtree(Node* node);

  // Moves what is pending into the committed set, if that is empty (otherwise pending goes on growing). Entries of nodes that are no
  // longer in a tree of the document are left out. True if there is a committed set now.
  bool Commit(Document* document);
  // The set being worked on, in the order the nodes were first changed.
  const std::vector<DamageEntry>& Committed() const { return committed_; }
  // The work on the committed set is done.
  void Finish() { committed_.clear(); }

  size_t PendingCount() const { return pending_.size(); }
  // How many changes were noted, however much they were merged: for telling that coalescing happens.
  uint64_t NotedCount() const { return noted_; }

 private:
  bool enabled_ = false;
  std::unordered_map<Node*, size_t> index_;  // into pending_
  std::vector<DamageEntry> pending_;
  std::vector<DamageEntry> committed_;
  uint64_t noted_ = 0;
};

// The tracker of the document the node belongs to, if its document has one that is enabled; the notes below do nothing otherwise.
DamageTracker* DamageOf(Node* node);
void NoteDamage(Node* node, uint8_t flags);
void ForgetDamage(Node* node);
// Starts recording for the document (a layout engine attaches this way) and returns the tracker.
DamageTracker& EnableDamage(Document* document);

}  // namespace solar::dom
