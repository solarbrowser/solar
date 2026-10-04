#pragma once

#include <optional>
#include <string>
#include <vector>

#include "solar/dom/Node.h"

// Mutation observers (https://dom.spec.whatwg.org/#mutation-observers): what the tree algorithms tell them,
// which queues the records of the observers that are interested and has a microtask give them out.
namespace solar::dom {

// Whether any node has an observer at all, which is what makes everything below worth doing.
bool HasMutationObservers();

// "queue a tree mutation record": a change of the children of `target`.
void QueueChildListRecord(Node* target, const std::vector<Node*>& added, const std::vector<Node*>& removed, Node* previousSibling, Node* nextSibling);
// "queue a mutation record" of attributes and of characterData.
void QueueAttributeRecord(Element* element, const std::string& name, const std::string& namespaceUri, const std::optional<std::string>& oldValue);
void QueueCharacterDataRecord(Node* node, const std::string& oldValue);
// The part of "remove" that leaves what the observers of a subtree watch with the node that is going.
void RegisterTransientObservers(Node* node, Node* oldParent);

// "signal a slot change": the slot gets a slotchange event when the observers' microtask runs.
void QueueSlotChange(Element* slot);

void DefineMutationClasses(Quanta::Context& ctx);

}  // namespace solar::dom
