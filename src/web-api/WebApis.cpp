/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/WebApis.h"

#include "solar/web/Scripts.h"

namespace solar::webapi {

std::string Script() {
  std::string all;
  for (const char* const* piece = web::kScriptWebApi; *piece; ++piece) all += *piece;
  return all;
}

}  // namespace solar::webapi
