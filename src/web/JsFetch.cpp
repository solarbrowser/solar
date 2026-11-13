/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <string>

#include "solar/net/Cors.h"
#include "solar/net/FetchHeaders.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/BlobUrls.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"
#include "solar/net/MimeType.h"
#include "solar/web/FetchOperation.h"
#include "solar/web/Integrity.h"
#include "solar/net/Orb.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Value;

namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

std::string_view Trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

}  // namespace

void FetchOperation::FailWith(const Value& reason, const std::string& message) {
  Context& ctx = host.context();
  if (!settled) {
    settled = true;
    qe::Call(ctx, reject.Get(), qe::Undefined(), qe::Args(&reason, 1));
  } else if (body && !body->complete && !body->failure) {
    body->failure = message;
    body->failureValue = std::make_shared<qe::Persistent>(ctx, reason);
    body->Wake(true);
  }
  if (host.config().afterScript) host.config().afterScript();
}

void FetchOperation::Done() {
  if (finished) return;
  finished = true;
  if (signal && abortListener) std::erase(signal->abortAlgorithms, abortListener);
  host.Finish(this);
}

void FetchOperation::OnResponseIn(const net::PageResponse& info) {
  if (settled) return;  // aborted, or failed already
  Context& ctx = host.context();
  if (!integrity.empty() && !integrityReleased) {
    // An opaque response cannot be checked; any other waits for its whole body.
    if (info.type == ResponseType::Opaque) {
      FailWith(MakeTypeError(ctx, "Failed to fetch: the integrity of an opaque response cannot be checked"), "integrity of an opaque response");
      handle.Cancel();
      return;
    }
    integrityBuffering = true;
    integrityHead = info;
    return;
  }
  JsResponse* r = NewResponse(ctx);
  if (info.type == ResponseType::OpaqueRedirect) {
    r->type = ResponseType::OpaqueRedirect;
    r->status = 0;
    r->hiddenStatus = info.hiddenStatus;
    r->hiddenLocation = info.hiddenLocation;
    r->url = url::Serialize(info.url);
    r->headers = NewResponseHeaders(ctx, {});
  } else if (info.type == ResponseType::Opaque) {
    r->type = ResponseType::Opaque;
    r->status = 0;
    r->headers = NewResponseHeaders(ctx, {});
  } else {
    r->type = info.type;
    responseStatus = info.status;
    r->status = info.status;
    r->statusText = info.statusText;
    r->url = url::Serialize(info.url);
    r->redirected = info.redirected;
    r->headers = NewResponseHeaders(ctx, info.headers);
    if (info.hasBody) {
      body = std::make_shared<BodyBuffer>();
      body->complete = false;
      r->body = body;
    }
  }
  r->NoteWrite(qe::FromObject(r->headers));
  response = r;
  responseValue = qe::Persistent(ctx, qe::FromObject(r));

  settled = true;
  Value value = qe::FromObject(r);
  qe::Call(ctx, resolve.Get(), qe::Undefined(), qe::Args(&value, 1));
  if (host.config().afterScript) host.config().afterScript();
}

void FetchOperation::OnBodyIn(std::span<const uint8_t> data) {
  if (integrityBuffering) {
    integrityBytes.append(reinterpret_cast<const char*>(data.data()), data.size());
    return;
  }
  if (body && !body->failure) {
    body->bytes.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (!body->watchers.empty()) {
      body->Wake(false);
      if (host.config().afterScript) host.config().afterScript();
    }
  }
}

void FetchOperation::OnEndIn() {
  if (integrityBuffering) {
    integrityBuffering = false;
    if (!IntegrityMatches(integrity, integrityBytes)) {
      FailWith(MakeTypeError(host.context(), "Failed to fetch: the body does not match the integrity metadata"), "the body does not match the integrity metadata");
      Done();
      return;
    }
    integrityReleased = true;
    OnResponseIn(integrityHead);
    if (body && !body->failure) body->bytes = std::move(integrityBytes);
  }
  if (body && !body->failure) {
    body->complete = true;
    qe::ReportExternalAllocation(body->bytes.size());
    body->Wake(true);
  }
  // (A resource of the page, for performance.getEntriesByType("resource") and the observers of it: scripts of the page read their own timings.)
  {
    Context& ctx = host.context();
    Value note = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarNoteResource");
    if (qe::IsObject(note)) {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      Value arguments[] = {qe::FromWtf8(ctx, address), qe::FromUtf8(ctx, "fetch"), qe::FromNumber(ms), qe::FromNumber(body ? static_cast<double>(body->bytes.size()) : 0.0), qe::FromNumber(responseStatus)};
      qe::Call(ctx, note, qe::Undefined(), qe::Args(arguments, 5));
      if (qe::HasException(ctx)) ctx.clear_exception();
    }
  }
  if (host.config().afterScript) host.config().afterScript();
  Done();
}

void FetchOperation::OnErrorIn(std::string_view message) {
  if (!abortedByScript && !finished) {
    // (The network service words what is wrong with the request; an abort is the page's own.)
    const std::string text = message == "aborted" ? std::string("Failed to fetch (aborted)") : std::string(message);
    FailWith(MakeTypeError(host.context(), text), text);
  }
  Done();
}

void FetchOperation::OnResponse(const net::PageResponse& info) {
  host.InRealm([&] { OnResponseIn(info); });
}
void FetchOperation::OnBody(std::span<const uint8_t> data) {
  host.InRealm([&] { OnBodyIn(data); });
}
void FetchOperation::OnEnd() {
  host.InRealm([&] { OnEndIn(); });
}
void FetchOperation::OnError(std::string_view message) {
  host.InRealm([&] { OnErrorIn(message); });
}

void CancelOperation(FetchOperation& operation) {
  if (operation.finished) return;
  operation.abortedByScript = true;  // the host is going away: nobody is left to tell
  operation.handle.Cancel();
  if (operation.body && !operation.body->complete) {
    operation.body->failure = "the page went away";
    operation.body->waiting.clear();
    operation.body->watchers.clear();
  }
  operation.Done();
}

namespace {

// The Range header of a request for a blob: "bytes=" and either or both ends (Fetch Standard, "parse a single range header value"). False where it is none.
bool ParseSingleRange(const std::string& text, std::optional<uint64_t>& start, std::optional<uint64_t>& end) {
  size_t i = 0;
  const auto space = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i; };
  const auto number = [&](std::optional<uint64_t>& out) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return;
    uint64_t value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
      value = value > (UINT64_MAX - digit) / 10 ? UINT64_MAX : value * 10 + digit;
      ++i;
    }
    out = value;
  };
  if (text.compare(0, 5, "bytes") != 0) return false;
  i = 5;
  space();
  if (i >= text.size() || text[i] != '=') return false;
  ++i;
  space();
  number(start);
  space();
  if (i >= text.size() || text[i] != '-') return false;
  ++i;
  space();
  number(end);
  space();
  if (i != text.size() || (!start && !end)) return false;
  return !(start && end && *start > *end);
}

}  // namespace

bool SliceBlobRange(const std::string& range, std::string& bytes, std::string& contentRange) {
  std::optional<uint64_t> start, end;
  if (!ParseSingleRange(range, start, end)) return false;
  const uint64_t size = bytes.size();
  uint64_t from, to;
  if (!start) {
    from = *end >= size ? 0 : size - *end;
    to = size ? size - 1 : 0;
  } else {
    from = *start;
    to = !end || *end >= size ? (size ? size - 1 : 0) : *end;
  }
  if (from >= size) return false;
  contentRange = "bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size);
  bytes = bytes.substr(from, to - from + 1);
  return true;
}

namespace {

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string PercentDecode(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() + 0 && HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0) {
      out += static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2]));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

// Forgiving base64 (https://infra.spec.whatwg.org/#forgiving-base64-decode); false where it cannot be decoded.
bool DecodeBase64(std::string_view text, std::string& out) {
  std::string clean;
  for (char c : text) if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') clean += c;
  if (clean.size() % 4 == 0) {
    if (clean.size() >= 2 && clean.compare(clean.size() - 2, 2, "==") == 0) clean.resize(clean.size() - 2);
    else if (!clean.empty() && clean.back() == '=') clean.pop_back();
  }
  if (clean.size() % 4 == 1) return false;
  uint32_t bits = 0;
  int count = 0;
  for (char c : clean) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else return false;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out += static_cast<char>((bits >> count) & 0xFF);
    }
  }
  return true;
}

// The bytes of a data: address, and its type (https://fetch.spec.whatwg.org/#data-url-processor); false where it is not one that can be read.
bool DecodeDataUrl(const std::string& address, std::string& bytes, std::string& type) {
  if (address.rfind("data:", 0) != 0) return false;
  std::string rest = address.substr(5);
  if (const size_t hash = rest.find('#'); hash != std::string::npos) rest.resize(hash);
  const size_t comma = rest.find(',');
  if (comma == std::string::npos) return false;
  std::string mime = rest.substr(0, comma);
  const std::string payload = PercentDecode(std::string_view(rest).substr(comma + 1));
  // (white space at the ends of the type goes; a trailing ;base64 is the encoding, with only spaces before the word)
  const auto isWhitespace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; };
  while (!mime.empty() && isWhitespace(mime.back())) mime.pop_back();
  size_t lead = 0;
  while (lead < mime.size() && isWhitespace(mime[lead])) ++lead;
  mime.erase(0, lead);
  bool base64 = false;
  if (mime.size() >= 6) {
    std::string tail = mime.substr(mime.size() - 6);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tail == "base64") {
      size_t end = mime.size() - 6;
      while (end > 0 && mime[end - 1] == ' ') --end;
      if (end > 0 && mime[end - 1] == ';') {
        base64 = true;
        mime.resize(end - 1);
      }
    }
  }
  if (mime.starts_with(";")) mime = "text/plain" + mime;
  if (const auto record = net::ParseMimeType(mime)) type = net::SerializeMimeType(*record);
  else type = "text/plain;charset=US-ASCII";
  bytes.clear();
  if (base64) return DecodeBase64(payload, bytes);
  bytes = payload;
  return true;
}

Value Fetch(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (!qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "fetch is not a constructor");
    return qe::Undefined();
  }
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  const auto reject = [&](const Value& reason) {
    qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&reason, 1));
    return capability.promise;
  };
  const auto rejectWithPending = [&] {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return reject(error);
  };

  FetchHost* host = HostOf(ctx);
  if (!host) return reject(MakeTypeError(ctx, "Failed to fetch: this realm has no network"));
  if (args.empty()) return reject(MakeTypeError(ctx, "Failed to execute 'fetch' on 'Window': 1 argument required, but only 0 present."));

  JsRequest* request = MakeRequest(ctx, args[0], args.size() > 1 ? args[1] : qe::Undefined());
  if (!request) return rejectWithPending();
  if (request->signal->aborted) return reject(request->signal->reason);
  // A blob and a data address are not the network's: what they name is here already.
  if (request->url.scheme == "blob" || request->url.scheme == "data") {
    if (request->url.scheme == "blob" && request->method != "GET") return reject(MakeTypeError(ctx, "Failed to fetch: a blob address answers GET only"));
    url::Url withoutFragment = request->url;
    withoutFragment.fragment.reset();
    const std::string address = url::Serialize(withoutFragment);
    std::string bytes, type;
    bool found = false;
    if (request->url.scheme == "blob") {
      if (const auto entry = web::LookupBlobUrl(address)) {
        bytes = *entry->data;
        type = entry->type;
        found = true;
      }
    } else {
      found = DecodeDataUrl(url::Serialize(request->url), bytes, type);
    }
    if (!found) return reject(MakeTypeError(ctx, "Failed to fetch: the " + request->url.scheme + " address cannot be read"));
    JsResponse* r = NewResponse(ctx);
    r->type = ResponseType::Basic;
    r->status = 200;
    r->statusText = "OK";
    r->url = address;
    std::vector<std::pair<std::string, std::string>> list;
    std::string contentRange;
    // A blob answers a request for a range of it with that part.
    if (request->url.scheme == "blob") {
      if (const auto range = request->headers->headers.Get("Range")) {
        if (!SliceBlobRange(*range, bytes, contentRange)) return reject(MakeTypeError(ctx, "Failed to fetch: the Range of a blob request is not valid"));
        r->status = 206;
        r->statusText = "Partial Content";
      }
    }
    if (!type.empty() || request->url.scheme == "blob") list.emplace_back("Content-Type", type);
    if (request->url.scheme == "blob") list.emplace_back("Content-Length", std::to_string(bytes.size()));
    if (!contentRange.empty()) list.emplace_back("Content-Range", contentRange);
    r->headers = NewResponseHeaders(ctx, list);
    auto buffer = std::make_shared<BodyBuffer>();
    buffer->complete = true;
    buffer->bytes = std::move(bytes);
    r->body = buffer;
    r->NoteWrite(qe::FromObject(r->headers));
    Value value = qe::FromObject(r);
    qe::Call(ctx, capability.resolve, qe::Undefined(), qe::Args(&value, 1));
    return capability.promise;
  }
  if (request->url.scheme != "http" && request->url.scheme != "https") return reject(MakeTypeError(ctx, "Failed to fetch: unsupported scheme " + request->url.scheme));

  const url::Url& page = host->config().pageUrl;
  auto operation = std::make_unique<FetchOperation>(*host);
  FetchOperation* op = operation.get();
  op->mode = request->mode;
  op->address = url::Serialize(request->url, true);

  op->resolve = qe::Persistent(ctx, capability.resolve);
  op->reject = qe::Persistent(ctx, capability.reject);
  if (request->signal) {
    op->signal = request->signal;
    op->signalValue = qe::Persistent(ctx, qe::FromObject(request->signal));
    op->abortListener = std::make_shared<Listener>();
    op->abortListener->native = [op](Context&, JsEvent*) {
      if (op->finished) return;
      op->abortedByScript = true;
      op->FailWith(op->signal->reason, "aborted");
      op->handle.Cancel();
      op->Done();
    };
    op->signal->abortAlgorithms.push_back(op->abortListener);
  }

  op->integrity = request->integrity;
  host->Adopt(std::move(operation));

  // From here on the request needs its bytes, which a stream gives only once it has ended. What the network is asked is the request as the Fetch Standard has it, with what
  // the page knows of it: the rules of fetching are the network service's.
  auto launch = [host, op, request, page = page](std::shared_ptr<const std::string> bodyBytes) mutable {
    net::PageRequest asked;
    asked.url = request->url;
    asked.method = request->method;
    asked.headers = request->headers->headers.list();
    asked.body = std::move(bodyBytes);
    asked.mode = request->mode;
    asked.credentials = request->credentials;
    asked.redirect = static_cast<int>(request->redirect);
    asked.cache = request->cache;
    asked.sendsReferrer = !request->referrer.empty();
    asked.referrerPolicy = request->referrerPolicy;
    asked.pageUrl = page;
    asked.tls = host->config().tls;
    op->handle = host->config().client->StartFetch(std::move(asked), *op);
  };

  if (request->body && request->body->fromStream) {
    auto hold = std::make_shared<qe::Persistent>(ctx, qe::FromObject(request));
    auto run = std::make_shared<decltype(launch)>(std::move(launch));
    const bool started = ReadBodyStream(
        ctx, request,
        [host, op, hold, run](std::string bytes) {
          if (!host->Owns(op) || op->finished) return;
          (*run)(std::make_shared<const std::string>(std::move(bytes)));
        },
        [host, op, hold](const Value& reason) {
          if (!host->Owns(op) || op->finished) return;
          op->FailWith(reason, "the request body failed");
          op->Done();
        });
    if (!started) {
      op->abortedByScript = true;
      op->Done();
      return rejectWithPending();
    }
    return capability.promise;
  }
  std::shared_ptr<const std::string> bodyBytes;
  if (request->body) {
    // The request's bytes go as they are, shared with it, and it is read now.
    bodyBytes = std::shared_ptr<const std::string>(request->body, &request->body->bytes);
    request->used = true;
  }
  launch(std::move(bodyBytes));
  return capability.promise;
}

}  // namespace

void DefineFetchFunction(Context& ctx) { qe::DefineGlobalFunction(ctx, "fetch", Fetch, 1); }

}  // namespace solar::web
