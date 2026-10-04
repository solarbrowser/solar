#include "solar/html/Parser.h"

namespace solar::html {

void ParseDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting, std::vector<std::string>* errors) {
  TreeBuilder builder(ctx, document, markup, scripting);
  builder.Run();
  if (errors) *errors = builder.Errors();
}

dom::DocumentFragment* ParseFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, ScriptingMode scripting, std::vector<std::string>* errors) {
  dom::Document* contextDocument = context->nodeDocument;
  dom::Document* document = dom::NewDocument(ctx, true);
  if (contextDocument) document->mode = contextDocument->mode;
  dom::DocumentFragment* fragment = dom::NewDocumentFragment(ctx, contextDocument ? contextDocument : document);
  TreeBuilder builder(ctx, document, markup, scripting);
  builder.SetUpFragment(context, fragment);
  builder.Run();
  if (errors) *errors = builder.Errors();
  return fragment;
}

}  // namespace solar::html
