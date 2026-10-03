#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

struct FetchArguments {
  const char* url = nullptr;
  const char* userAgent = nullptr;
  std::string method = "GET";
  std::optional<std::string> data;
  std::vector<std::pair<std::string, std::string>> headers;
};

int RunFetch(const FetchArguments& arguments) {
  std::optional<solar::url::Url> url = solar::url::Parse(solar::url::Normalize(arguments.url));
  if (!url) {
    std::fprintf(stderr, "invalid url: %s\n", arguments.url);
    return 1;
  }
  auto loop = solar::net::Loop::Create();
  if (!loop) {
    std::fprintf(stderr, "cannot start the event loop (is io_uring available?)\n");
    return 1;
  }
  PrintingHandler handler;
  solar::net::HttpClientOptions options;
  if (arguments.userAgent) options.userAgent = arguments.userAgent;
  solar::net::HttpClient client(*loop, options);
  solar::net::FetchOptions fetch;
  fetch.method = arguments.method;
  fetch.headers = arguments.headers;
  if (arguments.data) fetch.body = std::make_shared<const std::string>(*arguments.data);
  client.Fetch(*url, handler, fetch);
  loop->Run();
  return handler.failed ? 1 : 0;
}

// fetch [-X method] [-d data] [-H "Name: value"]... [--user-agent text] url
std::optional<FetchArguments> ParseFetchArguments(int argc, char** argv) {
  FetchArguments out;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool takesValue = argument == "-X" || argument == "-d" || argument == "-H" || argument == "--user-agent";
    if (takesValue) {
      if (i + 1 >= argc) return std::nullopt;
      const char* value = argv[++i];
      if (argument == "-X") {
        out.method = value;
      } else if (argument == "-d") {
        out.data = value;
        if (out.method == "GET") out.method = "POST";  // as curl has it
      } else if (argument == "-H") {
        const std::string header = value;
        const size_t colon = header.find(':');
        if (colon == std::string::npos) return std::nullopt;
        size_t start = colon + 1;
        while (start < header.size() && header[start] == ' ') ++start;
        out.headers.emplace_back(header.substr(0, colon), header.substr(start));
      } else {
        out.userAgent = value;
      }
    } else if (!out.url) {
      out.url = argv[i];
    } else {
      return std::nullopt;
    }
  }
  if (!out.url) return std::nullopt;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const auto usage = [&] {
    std::fprintf(stderr,
                 "usage: %s <url>\n       %s fetch [-X <method>] [-d <data>] [-H \"Name: value\"]... [--user-agent <text>] <url>\n",
                 argv[0], argv[0]);
    return 2;
  };
  if (argc >= 2 && std::string(argv[1]) == "fetch") {
    const std::optional<FetchArguments> arguments = ParseFetchArguments(argc, argv);
    return arguments ? RunFetch(*arguments) : usage();
  }
  if (argc != 2) return usage();

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
