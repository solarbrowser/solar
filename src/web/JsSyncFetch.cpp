/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <memory>
#include <string>
#include <vector>

#include "solar/net/FetchHeaders.h"
#include "solar/net/HttpClient.h"
#include "solar/net/Loop.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/BlobUrls.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

// What a request that is waited for hears: the response in one piece.
struct Collector : net::FetchHandler {
  bool done = false, failed = false;
  net::HttpResponseHead head;
  std::string body;
  void OnResponseHead(const net::HttpResponseHead& response, const url::Url&) override { head = response; }
  void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override { done = true; }
  void OnError(std::string_view) override { done = failed = true; }
};

// The bytes a string of code points below 256 stands for (a request is written that way in script).
std::string FromIsomorphic(const std::string& utf8) {
  std::string out;
  for (size_t i = 0; i < utf8.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) { out.push_back(static_cast<char>(((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F))); ++i; }
  }
  return out;
}

// __solarSyncFetch(method, url, [name, value, ...], body): the request, sent and waited for, which is how a synchronous XMLHttpRequest goes.
// The answer is [status, reason, [name, value, ...], bytes], or null where the request failed.
Value SyncFetch(Context& ctx, Value, qe::Args args, Value) {
  FetchHost* host = HostOf(ctx);
  if (!host || args.size() < 4 || !host->config().client || !host->config().loop) return qe::Null();
  const std::string method = qe::ToWtf8(ctx, args[0]);
  const std::string address = qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Null();
  const auto parsed = url::Parse(address);
  if (!parsed) return qe::Null();
  if (parsed->scheme == "blob") {
    // What a blob address names is here already; a request for a range of it is answered with that part.
    url::Url bare = *parsed;
    bare.fragment.reset();
    const auto entry = web::LookupBlobUrl(url::Serialize(bare));
    if (!entry || method != "GET") return qe::Null();
    std::string bytes = *entry->data, contentRange;
    int status = 200;
    const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, args[2], "length"));
    for (uint32_t i = 0; i + 1 < length; i += 2) {
      std::string name = qe::ToWtf8(ctx, qe::Get(ctx, args[2], std::to_string(i).c_str()));
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name != "range") continue;
      if (!SliceBlobRange(FromIsomorphic(qe::ToWtf8(ctx, qe::Get(ctx, args[2], std::to_string(i + 1).c_str()))), bytes, contentRange)) return qe::Null();
      status = 206;
    }
    Value out = qe::NewArray(ctx);
    qe::ArrayPush(ctx, out, qe::FromUint32(static_cast<uint32_t>(status)));
    qe::ArrayPush(ctx, out, qe::FromUtf8(ctx, status == 206 ? "Partial Content" : "OK"));
    Value headers = qe::NewArray(ctx);
    const auto add = [&](const char* name, const std::string& value) { qe::ArrayPush(ctx, headers, qe::FromUtf8(ctx, name)); qe::ArrayPush(ctx, headers, qe::FromUtf8(ctx, value)); };
    add("Content-Type", entry->type);
    add("Content-Length", std::to_string(bytes.size()));
    if (!contentRange.empty()) add("Content-Range", contentRange);
    qe::ArrayPush(ctx, out, headers);
    qe::ArrayPush(ctx, out, qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}));
    return out;
  }
  net::FetchOptions options;
  options.method = method;
  options.initiator = host->config().pageUrl;
  const uint32_t count = qe::ToUint32(ctx, qe::Get(ctx, args[2], "length"));
  for (uint32_t i = 0; i + 1 < count; i += 2) {
    options.headers.emplace_back(FromIsomorphic(qe::ToWtf8(ctx, qe::Get(ctx, args[2], std::to_string(i).c_str()))),
                                 FromIsomorphic(qe::ToWtf8(ctx, qe::Get(ctx, args[2], std::to_string(i + 1).c_str()))));
  }
  if (!qe::IsNull(args[3]) && !qe::IsUndefined(args[3])) options.body = std::make_shared<const std::string>(FromIsomorphic(qe::ToWtf8(ctx, args[3])));
  Collector collector;
  net::FetchHandle handle = host->config().client->Fetch(*parsed, collector, std::move(options));
  host->config().loop->RunUntil([&] { return collector.done; });
  if (!collector.done) { handle.Cancel(); return qe::Null(); }
  if (collector.failed) return qe::Null();
  Value out = qe::NewArray(ctx);
  qe::ArrayPush(ctx, out, qe::FromUint32(static_cast<uint32_t>(collector.head.status)));
  qe::ArrayPush(ctx, out, qe::FromUtf8(ctx, net::IsomorphicDecode(collector.head.reason)));
  Value headers = qe::NewArray(ctx);
  for (const auto& [name, value] : collector.head.headers) {
    qe::ArrayPush(ctx, headers, qe::FromUtf8(ctx, net::IsomorphicDecode(name)));
    qe::ArrayPush(ctx, headers, qe::FromUtf8(ctx, net::IsomorphicDecode(value)));
  }
  qe::ArrayPush(ctx, out, headers);
  qe::ArrayPush(ctx, out, qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(collector.body.data()), collector.body.size()}));
  return out;
}

}  // namespace

void DefineSyncFetch(Context& ctx) { qe::DefineGlobalFunction(ctx, "__solarSyncFetch", SyncFetch, 4); }

}  // namespace solar::web
