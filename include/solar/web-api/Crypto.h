/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// crypto.subtle is written in script (src/web-api/js/crypto.js); the algorithms are OpenSSL's, and these are the natives the script reaches them by. Each takes and
// returns bytes (an ArrayBuffer or a view in, an ArrayBuffer out) and returns undefined where the operation failed, which the script turns into an OperationError.
namespace solar::webapi {

// Defines in a realm __solarCryptoDigest, __solarCryptoHmac, __solarCryptoAes, __solarCryptoPbkdf2, __solarCryptoHkdf, __solarCryptoGenerate,
// __solarCryptoSign, __solarCryptoVerify, __solarCryptoDerive, __solarCryptoPublic and the others the script lists.
void InstallCryptoNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
