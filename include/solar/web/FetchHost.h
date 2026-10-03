#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "quanta/Embed.h"
#include "solar/net/Cors.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Url.h"

namespace solar::web {

struct FetchOperation;

// What fetch() needs from the program around it: a client to send requests with, the page they are made
// on behalf of, and a way to run the script's promise jobs after the network has called into it.
//
// One per realm, made after the realm and gone before it (and before the Isolate): its destructor
// cancels what is still in flight, which needs the realm alive.
class FetchHost {
 public:
  struct Config {
    net::Loop* loop = nullptr;
    net::HttpClient* client = nullptr;
    // The page's URL: relative URLs in script resolve against it, and its origin is the origin of the
    // requests. A realm with no page can use about:blank, and then only absolute URLs work.
    url::Url pageUrl;
    // Called after the host has called into script (settling a promise, say), to run the promise jobs
    // that has queued: an Isolate's PerformMicrotaskCheckpoint.
    std::function<void()> afterScript;
  };

  FetchHost(Quanta::Context& ctx, Config config);
  ~FetchHost();

  FetchHost(const FetchHost&) = delete;
  FetchHost& operator=(const FetchHost&) = delete;

  const Config& config() const { return config_; }
  Quanta::Context& context() { return *ctx_; }

  // JSON.parse and JSON.stringify as they were when the realm was made, so that a page that replaces
  // them does not change what Response.json and json() do.
  Quanta::Value JsonParse() const { return jsonParse_.Get(); }
  Quanta::Value JsonStringify() const { return jsonStringify_.Get(); }

  // What CORS preflights have allowed, for every fetch of this realm.
  net::PreflightCache& preflights() { return preflights_; }

  // For fetch(): takes the operation, which then lives until it ends.
  void Adopt(std::unique_ptr<FetchOperation> operation);
  // Called when an operation has ended; it is destroyed once the stack it ended on has unwound.
  void Finish(FetchOperation* operation);

 private:
  Config config_;
  Quanta::Context* ctx_;
  Quanta::Embed::Persistent jsonParse_;
  net::PreflightCache preflights_;
  Quanta::Embed::Persistent jsonStringify_;
  std::vector<std::unique_ptr<FetchOperation>> operations_;
  // What a deferred task checks before it touches the host, which may be gone by then.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// The host of the realm the calling native belongs to; null if the realm has none.
FetchHost* HostOf(Quanta::Context& ctx);

}  // namespace solar::web
