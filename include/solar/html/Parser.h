/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"
#include "solar/html/TreeBuilder.h"

namespace solar::html {

// Parses `markup` into `document` (which is empty), as a page's HTML is. The parse errors go to `errors`
// if it is given. `scriptHandler`, if given, is called at each script element to run it, and the
// parser waits for it.
void ParseDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting = ScriptingMode::Disabled,
                   std::vector<std::string>* errors = nullptr, ScriptHandler scriptHandler = nullptr, bool allowDeclarativeShadowRoots = true);

// The same for markup that comes in pieces, as a page does from the network: `next` is asked for the next piece (and may wait for it); it returns false when the piece it gave is the
// last (or there is none). The tree is made as far as what has come goes, and goes on when the next piece is here; a piece may end anywhere (a character or a token cut in two is
// held until the rest comes).
void ParseDocumentStreaming(Quanta::Context& ctx, dom::Document* document, const std::function<bool(std::string&)>& next, ScriptingMode scripting = ScriptingMode::Disabled,
                            ScriptHandler scriptHandler = nullptr, bool allowDeclarativeShadowRoots = true);

// The HTML fragment parsing algorithm: the nodes `markup` is, in the context of `context`, as the children of
// a new DocumentFragment. It is what innerHTML and insertAdjacentHTML are made of.
dom::DocumentFragment* ParseFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, ScriptingMode scripting = ScriptingMode::Inert,
                                     std::vector<std::string>* errors = nullptr, bool allowDeclarativeShadowRoots = false);

}  // namespace solar::html
