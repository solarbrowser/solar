#pragma once

#include <string>
#include <vector>

#include "solar/dom/Node.h"

namespace solar::html {

// The HTML fragment serialization algorithm (https://html.spec.whatwg.org/#serializing-html-fragments): the
// markup of the children of `node`, which is what innerHTML reads. A noscript element's text is literal
// when `scripting` is on, which is whether scripting is enabled for the node's document.
std::string SerializeChildren(const dom::Node* node, bool scripting = false);
// ... with the shadow roots of the hosts under it that are serializable, or that are named in `shadowRoots`,
// written as the templates that make them again.
std::string SerializeChildrenWithShadowRoots(const dom::Node* node, bool serializableShadowRoots, const std::vector<const dom::ShadowRoot*>& shadowRoots, bool scripting = false);
// The markup of `node` itself and what is under it, which is what outerHTML reads.
std::string SerializeNode(const dom::Node* node, bool scripting = false);

}  // namespace solar::html
