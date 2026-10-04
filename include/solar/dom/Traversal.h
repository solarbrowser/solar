#pragma once

#include "solar/dom/Node.h"

namespace solar::dom {

// NodeIterators are live: the node they are at must not be lost to a removal.
bool HasNodeIterators();
void NodeIteratorsBeforeRemove(Node* node);

void DefineTraversalClasses(Quanta::Context& ctx);

}  // namespace solar::dom
