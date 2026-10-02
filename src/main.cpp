#include <cstdio>
#include <optional>
#include <string>

#include "solar/url/Normalizer.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <url>\n", argv[0]);
    return 2;
  }

  std::string normalized = solar::url::Normalize(argv[1]);
  std::optional<solar::url::Url> url = solar::url::Parse(normalized);
  if (!url) {
    std::fprintf(stderr, "invalid url: %s\n", normalized.c_str());
    return 1;
  }

  std::printf("href      %s\n", solar::url::Serialize(*url).c_str());
  std::printf("scheme    %s\n", url->scheme.c_str());
  if (url->IncludesCredentials()) {
    std::printf("username  %s\n", url->username.c_str());
    std::printf("password  %s\n", url->password.c_str());
  }
  if (url->host) std::printf("host      %s\n", url->host->c_str());
  if (url->port) std::printf("port      %u\n", *url->port);
  if (url->opaquePath) {
    std::printf("path      %s\n", url->opaquePath->c_str());
  } else {
    std::string path;
    for (const std::string& segment : url->path) path += "/" + segment;
    std::printf("path      %s\n", path.c_str());
  }
  if (url->query) std::printf("query     %s\n", url->query->c_str());
  if (url->fragment) std::printf("fragment  %s\n", url->fragment->c_str());
  return 0;
}
