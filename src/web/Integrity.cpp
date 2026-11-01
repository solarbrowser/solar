/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web/Integrity.h"

#include <openssl/evp.h>

#include <vector>

namespace solar::web {

namespace {

std::string Base64(const unsigned char* data, size_t size) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < size; i += 3) {
    unsigned value = data[i] << 16;
    if (i + 1 < size) value |= data[i + 1] << 8;
    if (i + 2 < size) value |= data[i + 2];
    out.push_back(alphabet[(value >> 18) & 63]);
    out.push_back(alphabet[(value >> 12) & 63]);
    if (i + 1 < size) out.push_back(alphabet[(value >> 6) & 63]);
    if (i + 2 < size) out.push_back(alphabet[value & 63]);
  }
  return out;  // (without the padding: the metadata's is taken off to compare)
}

std::string Normalize(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c == '=') continue;
    out.push_back(c == '-' ? '+' : c == '_' ? '/' : c);
  }
  return out;
}

struct Hash {
  int strength;  // 1 for sha256, 2 for sha384, 3 for sha512
  std::string digest;
};

}  // namespace

bool IntegrityMatches(std::string_view metadata, std::string_view bytes) {
  std::vector<Hash> hashes;
  size_t position = 0;
  while (position < metadata.size()) {
    while (position < metadata.size() && (metadata[position] == ' ' || metadata[position] == '\t' || metadata[position] == '\n' || metadata[position] == '\r' || metadata[position] == '\f')) ++position;
    size_t end = position;
    while (end < metadata.size() && metadata[end] != ' ' && metadata[end] != '\t' && metadata[end] != '\n' && metadata[end] != '\r' && metadata[end] != '\f') ++end;
    std::string_view token = metadata.substr(position, end - position);
    position = end;
    if (token.empty()) continue;
    if (const size_t options = token.find('?'); options != std::string_view::npos) token = token.substr(0, options);
    const size_t dash = token.find('-');
    if (dash == std::string_view::npos || dash + 1 >= token.size()) continue;
    std::string algorithm(token.substr(0, dash));
    for (char& c : algorithm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int strength = algorithm == "sha256" ? 1 : algorithm == "sha384" ? 2 : algorithm == "sha512" ? 3 : 0;
    if (strength == 0) continue;
    hashes.push_back({strength, Normalize(token.substr(dash + 1))});
  }
  if (hashes.empty()) return true;
  int strongest = 0;
  for (const Hash& hash : hashes) strongest = std::max(strongest, hash.strength);
  const EVP_MD* md = strongest == 1 ? EVP_sha256() : strongest == 2 ? EVP_sha384() : EVP_sha512();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, md, nullptr) != 1) return false;
  const std::string computed = Base64(digest, length);
  for (const Hash& hash : hashes) {
    if (hash.strength == strongest && hash.digest == computed) return true;
  }
  return false;
}

}  // namespace solar::web
