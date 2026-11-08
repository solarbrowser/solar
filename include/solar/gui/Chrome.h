/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

// The browser's own interface (the bar with the buttons and the address) is a page, drawn by a renderer process of its own like the pages are, from a document the browser keeps
// (src/gui/ui/chrome.html). It is above the page in the window; the page is the rest.
namespace solar::gui {

// The height of the bar, in points.
constexpr int kChromeHeight = 88;
// (Of which the strip of the tabs, above the bar of the address.)
constexpr int kTabStripHeight = 40;
// The address that document is at (the browser answers a navigation to it for the interface's process, and for no other).
constexpr const char* kChromeAddress = "solar://chrome/";
// The page of a new tab: the browser's own, served from its memory to the renderer that is made for it.
constexpr const char* kNewTabAddress = "solar://newtab/";

// The document of the interface, and of a new tab.
std::string ChromeDocument();
std::string NewTabDocument();

// What the user typed in the address bar, as an address: one with a scheme as it is, a host as an https address, anything else a search.
std::string AddressFromInput(const std::string& text);

}  // namespace solar::gui
