#include "solar/html/Parser.h"

namespace solar::html {

void ParseDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting, std::vector<std::string>* errors, ScriptHandler scriptHandler, bool allowDeclarativeShadowRoots) {
  TreeBuilder builder(ctx, document, markup, scripting);
  builder.SetAllowDeclarativeShadowRoots(allowDeclarativeShadowRoots);
  if (scriptHandler) builder.SetScriptHandler(std::move(scriptHandler));
  builder.Run();
  if (errors) *errors = builder.Errors();
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
