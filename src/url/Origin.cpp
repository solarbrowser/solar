#include "solar/url/Origin.h"

#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::url {

std::string SerializeOrigin(const Url& url) {
  if (url.scheme == "blob") {
    // There is no blob URL store yet, so the entry the standard checks first is always null.
    std::optional<Url> inner = Parse(SerializePath(url));
    if (inner && (inner->scheme == "http" || inner->scheme == "https")) return SerializeOrigin(*inner);
    return "null";
  }

  if (url.scheme == "http" || url.scheme == "https" || url.scheme == "ftp" || url.scheme == "ws" ||
      url.scheme == "wss") {
    std::string out = url.scheme + "://" + url.host.value_or("");
    if (url.port) out += ":" + std::to_string(*url.port);
    return out;
  }

  // file is left to the implementation by the standard; treating it as opaque is its advice.
  return "null";
}

}  // namespace solar::url
