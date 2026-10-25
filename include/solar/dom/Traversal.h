/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "solar/dom/Node.h"

namespace solar::dom {

// NodeIterators are live: the node they are at must not be lost to a removal.
bool HasNodeIterators();
void NodeIteratorsBeforeRemove(Node* node);

void DefineTraversalClasses(Quanta::Context& ctx);

}  // namespace solar::dom
