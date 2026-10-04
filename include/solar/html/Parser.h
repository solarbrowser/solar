#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"
#include "solar/html/TreeBuilder.h"

namespace solar::html {

// Parses `markup` into `document` (which is empty), as a page's HTML is. The parse errors go to `errors`
// if it is given.
void ParseDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting = ScriptingMode::Disabled,
                   std::vector<std::string>* errors = nullptr);

// The HTML fragment parsing algorithm: the nodes `markup` is, in the context of `context`, as the children of
// a new DocumentFragment. It is what innerHTML and insertAdjacentHTML are made of.
dom::DocumentFragment* ParseFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, ScriptingMode scripting = ScriptingMode::Inert,
                                     std::vector<std::string>* errors = nullptr);

}  // namespace solar::html
