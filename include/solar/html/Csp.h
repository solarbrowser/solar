#pragma once

#include <string>
#include <string_view>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"

// Content Security Policy, the part that gates script: script-src (default-src when there is none) for the scripts a
// document runs, and for what is compiled from a string (eval, Function, string timers). Policies come from
// <meta http-equiv="Content-Security-Policy"> and from whoever loads the page (AddPolicy). Trusted Types and the
// securitypolicyviolation event are not done.
namespace solar::html::csp {

// Adds a serialized policy (the value of a Content-Security-Policy header) to the document.
void AddPolicy(dom::Document* document, std::string_view policy);

// Whether the document may compile strings: with `unsafe-eval` in its script sources, or no policy that limits them.
bool AllowsEval(const dom::Document* document);

// Whether a script of the document may run: an inline one by its nonce (the `nonce` attribute), or one at `address`.
bool AllowsInlineScript(const dom::Document* document, const std::string& nonce);
bool AllowsScriptUrl(const dom::Document* document, const std::string& address, const std::string& nonce);

// The hooks to give the Isolate: eval and Function refuse with an EvalError where the document's policy forbids them.
Quanta::Embed::CodeGenerationHooks MakeCodeGenerationHooks();

}  // namespace solar::html::csp
