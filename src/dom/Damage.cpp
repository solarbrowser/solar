#include "solar/dom/Damage.h"

namespace solar::dom {

void DamageTracker::Note(Node* node, uint8_t flags) {
  if (!enabled_ || !node) return;
  ++noted_;
  const auto found = index_.find(node);
  if (found != index_.end()) {
    pending_[found->second].flags |= flags;
    return;
  }
  index_.emplace(node, pending_.size());
  pending_.push_back({node, flags});
}

void DamageTracker::ForgetSubtree(Node* node) {
  if (!enabled_ || !node || (pending_.empty() && committed_.empty())) return;
  const auto inside = [node](Node* n) {
    for (; n; n = n->parentNode) {
      if (n == node) return true;
    }
    return false;
  };
  // (The node is already unlinked from its parent when this is called, so the walk up ends at it.)
  std::vector<DamageEntry> kept;
  kept.reserve(pending_.size());
  for (const DamageEntry& entry : pending_) {
    if (!inside(entry.node)) kept.push_back(entry);
  }
  if (kept.size() != pending_.size()) {
    pending_ = std::move(kept);
    index_.clear();
    for (size_t i = 0; i < pending_.size(); ++i) index_.emplace(pending_[i].node, i);
  }
  // The committed set belongs to the work in hand, which skips what is not in the tree any more.
  std::erase_if(committed_, [&](const DamageEntry& entry) { return inside(entry.node); });
}

bool DamageTracker::Commit(Document* document) {
  if (!committed_.empty()) return true;
  if (pending_.empty()) return false;
  committed_.reserve(pending_.size());
  for (const DamageEntry& entry : pending_) {
    Node* root = entry.node;
    while (root->parentNode) root = root->parentNode;
    if (root == document || entry.node == document) committed_.push_back(entry);
  }
  pending_.clear();
  index_.clear();
  return !committed_.empty();
}

DamageTracker* DamageOf(Node* node) {
  if (!node) return nullptr;
  Document* document = node->IsDocument() ? static_cast<Document*>(node) : node->nodeDocument;
  if (!document || !document->damage || !document->damage->Enabled()) return nullptr;
  return document->damage.get();
}

void NoteDamage(Node* node, uint8_t flags) {
  if (DamageTracker* tracker = DamageOf(node)) tracker->Note(node, flags);
}

void ForgetDamage(Node* node) {
  if (DamageTracker* tracker = DamageOf(node)) tracker->ForgetSubtree(node);
}

DamageTracker& EnableDamage(Document* document) {
  if (!document->damage) document->damage = std::make_shared<DamageTracker>();
  document->damage->Enable();
  return *document->damage;
}

}  // namespace solar::dom
