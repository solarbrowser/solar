/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/html/Parser.h"

namespace solar::html {

void ParseDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting, std::vector<std::string>* errors, ScriptHandler scriptHandler, bool allowDeclarativeShadowRoots) {
  TreeBuilder builder(ctx, document, markup, scripting);
  builder.SetAllowDeclarativeShadowRoots(allowDeclarativeShadowRoots);
  if (scriptHandler) builder.SetScriptHandler(std::move(scriptHandler));
  builder.Run();
  if (errors) *errors = builder.Errors();
}

namespace {
// How much of the bytes can go to the tokenizer now: not a character that is cut (the bytes of it that came are held for the rest), nor a carriage return (a line feed may follow it,
// and the two are one newline).
size_t SafeEnd(const std::string& bytes) {
  size_t end = bytes.size();
  if (end > 0 && bytes[end - 1] == '\r') --end;
  // A UTF-8 sequence that is not all there: its lead byte is within the last three, and wants more than follow it.
  for (size_t back = 1; back <= 3 && back <= end; ++back) {
    const unsigned char byte = static_cast<unsigned char>(bytes[end - back]);
    if ((byte & 0xC0) == 0x80) continue;  // (a continuation byte: the lead is before it)
    const size_t want = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
    if (want > back) end -= back;
    break;
  }
  return end;
}
}  // namespace

void ParseDocumentStreaming(Quanta::Context& ctx, dom::Document* document, const std::function<bool(std::string&)>& next, ScriptingMode scripting, ScriptHandler scriptHandler,
                            bool allowDeclarativeShadowRoots) {
  TreeBuilder builder(ctx, document, std::string_view(), scripting);
  builder.SetStreaming();
  builder.SetAllowDeclarativeShadowRoots(allowDeclarativeShadowRoots);
  if (scriptHandler) builder.SetScriptHandler(std::move(scriptHandler));
  std::string held, piece;
  bool more = true;
  for (;;) {
    builder.Run();
    if (builder.Done()) break;
    if (!more) {
      builder.Close();
      continue;
    }
    piece.clear();
    more = next(piece);
    held += piece;
    const size_t end = more ? SafeEnd(held) : held.size();
    if (end > 0) builder.Append(std::string_view(held).substr(0, end));
    held.erase(0, end);
  }
}

dom::DocumentFragment* ParseFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, ScriptingMode scripting, std::vector<std::string>* errors, bool allowDeclarativeShadowRoots) {
  dom::Document* contextDocument = context->nodeDocument;
  dom::Document* document = dom::NewDocument(ctx, true);
  if (contextDocument) {
    document->mode = contextDocument->mode;
    document->customElementsEnabled = contextDocument->window != nullptr;
  }
  dom::DocumentFragment* fragment = dom::NewDocumentFragment(ctx, contextDocument ? contextDocument : document);
  TreeBuilder builder(ctx, document, markup, scripting);
  builder.SetAllowDeclarativeShadowRoots(allowDeclarativeShadowRoots);
  builder.SetUpFragment(context, fragment);
  builder.Run();
  if (errors) *errors = builder.Errors();
  return fragment;
}

}  // namespace solar::html
