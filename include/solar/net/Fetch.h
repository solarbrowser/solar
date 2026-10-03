#pragma once

#include <chrono>
#include <span>
#include <string_view>

#include "solar/net/Http1Parser.h"
#include "solar/net/Loop.h"
#include "solar/url/Url.h"

namespace solar::net {

class FetchHandler {
 public:
  virtual ~FetchHandler() = default;

  virtual void OnResponseHead(const HttpResponseHead& head) = 0;
  // A view into the buffer the kernel filled, valid only until this returns.
  virtual void OnBody(std::span<const uint8_t> data) = 0;
  // Exactly one of OnEnd and OnError is called, and it is the last call.
  virtual void OnEnd() = 0;
  virtual void OnError(std::string_view message) = 0;
};

struct FetchOptions {
  // For the whole exchange, from connecting to the last byte of the body.
  std::chrono::milliseconds timeout{30000};
};

// Fetches `url` with a GET over plain HTTP/1.1. Everything is reported from Loop::Run, never
// from inside this call. `handler` must outlive the fetch.
void Fetch(Loop& loop, const url::Url& url, FetchHandler& handler, FetchOptions options = {});

}  // namespace solar::net
