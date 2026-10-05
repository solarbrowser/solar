#pragma once

#include <optional>
#include <string>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"

// ES modules in pages (https://html.spec.whatwg.org/#module-script): where the engine's module machinery asks the
// page for names and for sources. The hooks resolve a specifier with the document's import map, fetch what it names
// through the page's loader (and from data: and blob: URLs), and give a module its import.meta.
namespace solar::html {

// The hooks to give an Isolate with SetModuleHooks.
Quanta::Embed::ModuleHooks MakeModuleHooks();

// A realm's window document, which the hooks find the base URL and the import map in. InstallWindow registers it; the
// program that destroys the realm forgets it first.
void RegisterRealmDocument(Quanta::Embed::Realm* realm, dom::Document* document);
void ForgetRealm(Quanta::Embed::Realm* realm);

// "register an import map": the text of a <script type="importmap"> into the document's. False, with what was wrong.
bool RegisterImportMap(Quanta::Embed::Realm& realm, dom::Document* document, const std::string& json, std::string& error);

// What an inline module is called, which has no address of its own: the document's, made unique.
std::string InlineModuleUrl(dom::Document* document);
// The address with what makes an inline module unique taken off, which is the one script sees.
std::string VisibleModuleUrl(const std::string& url);

}  // namespace solar::html
