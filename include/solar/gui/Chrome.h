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

// The height of the bar, in points: the strip of the tabs and the bar of the address, when the tabs are along the top.
constexpr int kChromeHeight = 88;
// With the tabs at the side: the bar of the address alone is at the top, and the column of the tabs is this wide at the left.
constexpr int kToolbarHeight = 48;
constexpr int kSideWidth = 240;
// The column that has everything: this wide.
constexpr int kSidebarWidth = 272;
// (Of which the strip of the tabs, above the bar of the address.)
constexpr int kTabStripHeight = 40;
// The address that document is at (the browser answers a navigation to it for the interface's process, and for no other).
constexpr const char* kChromeAddress = "solar://chrome/";
// The same interface with the tabs in a column at the side (src/gui/ui/sidetabs-ui.html): the picture of it is the whole window, clear where the page is.
constexpr const char* kSideTabsAddress = "solar://sidetabs/";
// And with everything in the column (the buttons, the address, the tabs; no bar along the top): src/gui/ui/sidebar-ui.html.
constexpr const char* kSidebarAddress = "solar://sidebar/";
// The page of a new tab: the browser's own, served from its memory to the renderer that is made for it.
constexpr const char* kNewTabAddress = "solar://newtab/";
// The settings page: the browser's own too, a renderer of its own that may ask the browser to change a setting (and nothing else).
constexpr const char* kSettingsAddress = "solar://settings/";

// The icons that were embedded when the program was built (assets/icons): the name, and the svg, drawn in the colour of the text. The table ends with a null name.
struct Icon {
  const char* name;
  const char* svg;
};
extern const Icon kIcons[];

// The document of the interface, and of a new tab. In the interface's, {{icon:name}} is the icon of that name.
std::string ChromeDocument();
std::string SideTabsDocument();
std::string SidebarDocument();
// Whether the address is of one of the two layouts of the interface (the process that has it draws the bar, may ask the browser for things, and its picture has parts that are clear).
bool IsInterfaceAddress(const std::string& address);
std::string NewTabDocument();
std::string SettingsDocument();

// What the user typed in the address bar, as an address: one with a scheme as it is, a host as an https address, anything else a search (`searchAddress` is where the words are put after).
std::string AddressFromInput(const std::string& text, const std::string& searchAddress = "https://html.duckduckgo.com/html/?q=");

}  // namespace solar::gui
