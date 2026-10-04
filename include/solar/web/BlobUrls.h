#pragma once

#include <memory>
#include <optional>
#include <string>

namespace solar::web {

// The blob: URLs that URL.createObjectURL has made and not yet revoked (https://w3c.github.io/FileAPI/#BlobURL):
// each names the bytes of a Blob, which whatever loads from the address is given. One store for the thread.
struct BlobUrlEntry {
  std::shared_ptr<const std::string> data;
  std::string type;
};

// Makes the address for the bytes: "blob:" and the origin it was made in, then a random identifier.
std::string RegisterBlobUrl(const std::string& origin, BlobUrlEntry entry);
std::optional<BlobUrlEntry> LookupBlobUrl(const std::string& url);
void RevokeBlobUrl(const std::string& url);

}  // namespace solar::web
