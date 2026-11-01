/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

namespace solar::html {

// The name of the encoding a label stands for (utf8 is UTF-8, 866 is IBM866), or nothing for a label that is none.
std::string EncodingNameOfLabel(const std::string& label);

}  // namespace solar::html
