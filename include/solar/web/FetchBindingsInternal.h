#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quanta/Embed.h"
#include "solar/net/FetchHeaders.h"
#include "solar/net/HttpCache.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Url.h"
#include "solar/web/DomBindingsInternal.h"

namespace solar::web {

// A Headers object: a header list with the guard it was made with.
struct JsHeaders : Quanta::DOMObject {
  net::FetchHeaders headers;
  // Held for the reason the URL objects hold theirs: what a Headers can outlive its realm with.
  Quanta::Object* iteratorPrototype = nullptr;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(iteratorPrototype); }
};

// A body: the bytes of a request's or a response's, which are still arriving for a response that is
// being fetched. They are kept whole, as the network gave them (the client has already undone any
// content coding), and the same bytes serve a response and its clones.
struct BodyBuffer {
  std::string bytes;
  bool complete = true;
  std::optional<std::string> failure;  // why it will never be complete
  // What a read of it rejects with when it was cut short on purpose (an abort), instead of a TypeError.
  std::shared_ptr<Quanta::Embed::Persistent> failureValue;
  std::vector<std::function<void()>> waiting;  // consumers that asked before it was complete
};

// The part of Request and Response that is the Body mixin of the Fetch Standard.
struct JsBodyOwner : Quanta::DOMObject {
  std::shared_ptr<BodyBuffer> body;  // none: the object has no body
  bool used = false;                 // the body has been read (or handed over)
  void Visit(Quanta::Visitor&) {}
};

// A Blob: immutable bytes with a type. A slice shares its parent's bytes.
struct JsBlob : Quanta::DOMObject {
  std::shared_ptr<const std::string> data;
  size_t offset = 0;
  size_t size = 0;
  std::string type;  // lower case, printable ASCII, or empty

  std::string_view Bytes() const { return data ? std::string_view(*data).substr(offset, size) : std::string_view(); }
  void Visit(Quanta::Visitor&) {}
};

struct JsFile : JsBlob {
  using Parent = JsBlob;
  std::string name;
  int64_t lastModified = 0;  // milliseconds since the epoch
  void Visit(Quanta::Visitor&) {}
};

// A new Blob in the realm of `ctx` holding a copy of `bytes`.
JsBlob* NewBlob(Quanta::Context& ctx, std::string bytes, std::string type);
// A new File; a negative `lastModified` means now.
JsFile* NewFile(Quanta::Context& ctx, std::string bytes, std::string name, std::string type, int64_t lastModified);
void DefineBlobClasses(Quanta::Context& ctx);
// TextEncoder and TextDecoder (the Encoding Standard), for UTF-8, UTF-16 and windows-1252.
void DefineEncodingClasses(Quanta::Context& ctx);

// One entry of a FormData: a name and either a string or a File.
struct FormDataEntry {
  std::string name;
  std::string value;
  JsFile* file = nullptr;
};

struct JsFormData : Quanta::DOMObject {
  std::vector<FormDataEntry> entries;
  Quanta::Object* iteratorPrototype = nullptr;
  void Visit(Quanta::Visitor& visitor) {
    for (const FormDataEntry& entry : entries) visitor.Mark(entry.file);
    visitor.Mark(iteratorPrototype);
  }
};

JsFormData* NewFormData(Quanta::Context& ctx);
void DefineFormDataClass(Quanta::Context& ctx);
// A FormData as a body: multipart/form-data, and the Content-Type with its boundary.
void EncodeMultipart(const JsFormData& form, std::string& body, std::string& contentType);
// A body as a FormData, by its Content-Type: urlencoded or multipart. False if it is neither or is malformed.
bool ParseFormData(Quanta::Context& ctx, std::string_view body, std::string_view contentType, JsFormData* into);

enum class ResponseType { Basic, Cors, Default, Error, Opaque, OpaqueRedirect };

struct JsResponse : JsBodyOwner {
  using Parent = JsBodyOwner;
  ResponseType type = ResponseType::Default;
  int status = 200;
  std::string statusText;
  std::string url;  // serialized without the fragment; empty if it has none
  bool redirected = false;
  JsHeaders* headers = nullptr;
  void Visit(Quanta::Visitor& visitor) { visitor.Mark(headers); }
};

enum class RequestMode { SameOrigin, NoCors, Cors, Navigate };
enum class RequestCredentials { Omit, SameOrigin, Include };

struct JsRequest : JsBodyOwner {
  using Parent = JsBodyOwner;
  std::string method = "GET";
  url::Url url;
  JsHeaders* headers = nullptr;
  RequestMode mode = RequestMode::Cors;
  RequestCredentials credentials = RequestCredentials::SameOrigin;
  net::CacheMode cache = net::CacheMode::Default;
  net::RedirectMode redirect = net::RedirectMode::Follow;
  std::string referrer = "about:client";  // "" for no referrer
  std::string referrerPolicy;
  std::string integrity;
  bool keepalive = false;
  JsAbortSignal* signal = nullptr;
  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(headers);
    visitor.Mark(signal);
  }
};

// new Request(input, init), as a request object and not a value; null, with an exception pending, if the
// standard makes it a TypeError. fetch() uses it as well.
JsRequest* MakeRequest(Quanta::Context& ctx, const Quanta::Value& input, const Quanta::Value& init);

// What a BodyInit makes: the bytes, and the Content-Type that goes with them if the caller gave none.
struct ExtractedBody {
  std::shared_ptr<BodyBuffer> buffer;
  std::string contentType;
};
// BodyInit -> bytes. False, with an exception pending, if it is a kind this build does not have.
bool ExtractBody(Quanta::Context& ctx, const Quanta::Value& init, ExtractedBody& out);

enum class BodyKind { Text, Json, ArrayBuffer, Bytes, Blob, FormData };
// text(), json(), arrayBuffer() and bytes(): a promise for the body read as that.
// `mimeType` is the Content-Type of the body's owner, which becomes the type of a Blob.
Quanta::Value ConsumeBody(Quanta::Context& ctx, JsBodyOwner* owner, BodyKind kind, const std::string& mimeType = "");
// A promise already resolved with `value`.
Quanta::Value ResolvedPromise(Quanta::Context& ctx, const Quanta::Value& value);
// UTF-8 decode of the Encoding Standard: a leading byte order mark dropped, malformed bytes replaced.
Quanta::Value DecodeUtf8(Quanta::Context& ctx, std::string_view bytes);
// A Blob's type from a Content-Type value: the MIME type in lower case, or empty if it is none.
std::string BlobTypeFromContentType(std::string_view contentType);

// A TypeError as a value, to reject a promise with.
Quanta::Value MakeTypeError(Quanta::Context& ctx, const std::string& message);
// A promise that is already rejected.
Quanta::Value RejectedPromise(Quanta::Context& ctx, const Quanta::Value& reason);

// Fills `target` from a HeadersInit (a sequence of pairs, a record, or another Headers). `what` starts
// the message of the TypeError that says it cannot. False, with an exception pending, if it did.
bool FillHeaders(Quanta::Context& ctx, JsHeaders* target, const Quanta::Value& init, const char* what);
// A Headers with the guard of a response, immutable, filled from a header list as it came off the wire.
JsHeaders* NewResponseHeaders(Quanta::Context& ctx, const std::vector<std::pair<std::string, std::string>>& list);

void DefineResponseClass(Quanta::Context& ctx);
// An empty Response of the realm of `ctx`, for fetch to fill in.
JsResponse* NewResponse(Quanta::Context& ctx);
void DefineRequestClass(Quanta::Context& ctx);
void DefineFetchFunction(Quanta::Context& ctx);
// Ends what is in flight for an operation that the host is going away with.
struct FetchOperation;
void CancelOperation(FetchOperation& operation);

void DefineHeadersClass(Quanta::Context& ctx);

// A Headers in the realm the calling native belongs to, empty and with `guard`.
JsHeaders* AllocateHeaders(Quanta::Context& ctx, net::HeadersGuard guard);

}  // namespace solar::web
