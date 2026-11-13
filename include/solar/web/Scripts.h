/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace solar::web {

// The scripts the Web APIs are partly written in, as pieces of one program each, ended by a null.
extern const char* const kScriptStreams[];
extern const char* const kScriptUiEvents[];
extern const char* const kScriptXhr[];
extern const char* const kScriptTypedOm[];
extern const char* const kScriptFontLoading[];
extern const char* const kScriptLayout[];
extern const char* const kScriptAnimations[];
extern const char* const kScriptWebApi[];  // (src/web-api/js)
extern const char* const kScriptFilterLists[];  // (assets/filters: the lists of the content blocker, one after the other)
extern const char* const kScriptDownloadsPage[];  // (src/gui/ui/downloads.html)
extern const char* const kScriptUiMenu[];  // (src/gui/ui/ui-menu.html: the menu, in the three layouts)
extern const char* const kScriptSidebarUi[];  // (src/gui/ui/sidebar-ui.html)
extern const char* const kScriptSideTabsUi[];  // (src/gui/ui/sidetabs-ui.html)
extern const char* const kScriptUiCommonCss[];  // (src/gui/ui/ui-common.css, in both layouts of the interface)
extern const char* const kScriptUiCommonJs[];  // (src/gui/ui/ui-common.js)
extern const char* const kScriptSettingsPage[];  // (src/gui/ui/settings.html)
extern const char* const kScriptNewTab[];  // (src/gui/ui/newtab.html)
extern const char* const kScriptChromeUi[];  // (src/gui/ui/chrome.html: a document, not a script)
extern const char* const kScriptWebApiWorker[];  // (src/web-api/worker-js)

}  // namespace solar::web
