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

}  // namespace solar::web
