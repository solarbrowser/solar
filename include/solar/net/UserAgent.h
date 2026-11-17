/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

namespace solar::net {

// The User-Agent a client sends when it has not been given one, and what a page's `navigator.userAgent` says (the two are the same). It is shaped as Chrome's is (a site that looks at the
// User-Agent looks for the engine's name, and Google's sign-in script is refused to one it does not know), with Solar's name at its end: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36
// (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36 Solar/1.0. The part in parentheses is the system the build was made for. SOLAR_USER_AGENT sets another.
std::string DefaultUserAgent();
// What `navigator.platform` says: Win32, MacIntel or Linux x86_64.
std::string NavigatorPlatform();
// The colour scheme the user wants (what prefers-color-scheme says to the pages), sent to https servers as Sec-CH-Prefers-Color-Scheme. A site that draws its theme on the server (YouTube) reads it;
// without it the page came light while matchMedia said dark, and YouTube took the difference for a toggle of the system's theme and reloaded the page with themeRefresh=1.
void SetPrefersDark(bool dark);
bool PrefersDark();

}  // namespace solar::net
