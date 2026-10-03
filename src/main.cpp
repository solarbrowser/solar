#include <cstdio>
#include <optional>
#include <string>

#include "solar/net/HttpClient.h"
#include "solar/url/Normalizer.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace {

class PrintingHandler : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const solar::net::HttpResponseHead& head, const solar::url::Url& finalUrl) override {
    std::printf("HTTP %d %s  (%s)\n", head.status, head.reason.c_str(), solar::url::Serialize(finalUrl).c_str());
    for (const auto& [name, value] : head.headers) std::printf("%s: %s\n", name.c_str(), value.c_str());
    std::printf("\n");
  }
  void OnBody(std::span<const uint8_t> data) override { std::fwrite(data.data(), 1, data.size(), stdout); }
  void OnEnd() override {}
  void OnError(std::string_view message) override {
    std::fprintf(stderr, "fetch failed: %.*s\n", static_cast<int>(message.size()), message.data());
    failed = true;
  }
  bool failed = false;
};

int RunFetch(const char* typed) {
  std::optional<solar::url::Url> url = solar::url::Parse(solar::url::Normalize(typed));
  if (!url) {
    std::fprintf(stderr, "invalid url: %s\n", typed);
    return 1;
  }
  auto loop = solar::net::Loop::Create();
  if (!loop) {
    std::fprintf(stderr, "cannot start the event loop (is io_uring available?)\n");
    return 1;
  }
  PrintingHandler handler;
  solar::net::HttpClient client(*loop);
  client.Fetch(*url, handler);
  loop->Run();
  return handler.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "fetch") return RunFetch(argv[2]);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <url>\n       %s fetch <url>\n", argv[0], argv[0]);
    return 2;
  }

  std::string normalized = solar::url::Normalize(argv[1]);
  solar::url::ValidationErrors errors;
  std::optional<solar::url::Url> url = solar::url::Parse(normalized, nullptr, &errors);
  for (solar::url::ValidationError error : errors) {
    std::fprintf(stderr, "warning: %s\n", solar::url::ValidationErrorName(error));
  }
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
