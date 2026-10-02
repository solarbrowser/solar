#include "solar/url/Serializer.h"

namespace solar::url {

std::string Serialize(const Url& url, bool excludeFragment) {
  std::string out = url.scheme + ":";

  if (url.host) {
    out += "//";
    if (url.IncludesCredentials()) {
      out += url.username;
      if (!url.password.empty()) out += ":" + url.password;
      out += '@';
    }
    out += *url.host;
    if (url.port) out += ":" + std::to_string(*url.port);
  }

  // Without this, a path starting with an empty segment would read back as a host.
  if (!url.host && !url.opaquePath && url.path.size() > 1 && url.path[0].empty()) out += "/.";

  if (url.opaquePath) {
    out += *url.opaquePath;
  } else {
    for (const std::string& segment : url.path) out += "/" + segment;
  }

  if (url.query) out += "?" + *url.query;
  if (!excludeFragment && url.fragment) out += "#" + *url.fragment;
  return out;
}

}  // namespace solar::url
