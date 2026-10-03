#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/UrlBindingsInternal.h"
#include "solar/web/WebIdl.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_blobPrototypeKey;
char g_filePrototypeKey;

Object* BlobPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_blobPrototypeKey)); }
Object* FilePrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_filePrototypeKey)); }

JsBlob* This(Context& ctx, const Value& thisValue) {
  JsBlob* self = DOMObject::Cast<JsBlob>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

// The type of a Blob: printable ASCII only, lower case; anything else makes it empty.
std::string NormalizeType(const std::string& type) {
  std::string out = type;
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return "";
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

// "native" line endings: LF written as the platform writes it, and CRLF and CR taken as LF first.
std::string NativeLineEndings(const std::string& text) {
#ifdef _WIN32
  const char* newline = "\r\n";
#else
  const char* newline = "\n";
#endif
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      out += newline;
    } else if (text[i] == '\n') {
      out += newline;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

// One of the blobParts of Blob and File, as it was when it was converted: a Blob's or buffer's bytes, or
// a string, whose line endings depend on an option that is read after the parts.
struct Part {
  bool isText;
  std::string content;
};

// The blobParts: a sequence of Blobs, buffers and strings, converted in order and before the options.
bool CollectParts(Context& ctx, const Value& parts, std::vector<Part>& out, const char* what) {
  if (!qe::IsObject(parts)) {
    qe::ThrowTypeError(ctx, std::string("Failed to construct '") + what + "': The provided value cannot be converted to a sequence.");
    return false;
  }
  Value method = qe::GetIteratorMethod(ctx, parts);
  if (qe::HasException(ctx)) return false;
  if (qe::IsUndefined(method)) {
    qe::ThrowTypeError(ctx, std::string("Failed to construct '") + what + "': The object must have a callable @@iterator property.");
    return false;
  }
  return Iterate(ctx, parts, method, [&](const Value& part) {
    if (JsBlob* blob = DOMObject::Cast<JsBlob>(part)) {
      out.push_back({false, std::string(blob->Bytes())});
    } else if (std::optional<std::span<const uint8_t>> bytes = qe::BytesOf(part)) {
      out.push_back({false, std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size())});
    } else {
      std::string text = qe::ToUsvUtf8(ctx, part);
      if (qe::HasException(ctx)) return false;
      out.push_back({true, std::move(text)});
    }
    return true;
  });
}

std::string Join(const std::vector<Part>& parts, bool nativeEndings) {
  std::string out;
  for (const Part& part : parts) out += part.isText && nativeEndings ? NativeLineEndings(part.content) : part.content;
  return out;
}

// BlobPropertyBag, and the FilePropertyBag that extends it.
struct PropertyBag {
  std::string type;
  bool nativeEndings = false;
  std::optional<int64_t> lastModified;
};

bool ReadPropertyBag(Context& ctx, const Value& options, bool forFile, PropertyBag& out, const char* what) {
  if (qe::IsUndefined(options) || qe::IsNull(options)) return true;
  if (!qe::IsObject(options)) {
    qe::ThrowTypeError(ctx, std::string("Failed to construct '") + what + "': The provided value is not of type '" + what + "PropertyBag'.");
    return false;
  }
  Value endings = qe::Get(ctx, options, "endings");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(endings)) {
    const std::string text = qe::ToUsvUtf8(ctx, endings);
    if (qe::HasException(ctx)) return false;
    if (text != "transparent" && text != "native") {
      qe::ThrowTypeError(ctx, std::string("Failed to construct '") + what + "': The provided value '" + text + "' is not a valid enum value of type EndingType.");
      return false;
    }
    out.nativeEndings = text == "native";
  }
  if (forFile) {
    Value lastModified = qe::Get(ctx, options, "lastModified");
    if (qe::HasException(ctx)) return false;
    if (!qe::IsUndefined(lastModified)) {
      double number = 0;
      if (!lastModified.to_number_checked(ctx, number)) return false;
      out.lastModified = std::isfinite(number) ? static_cast<int64_t>(number) : 0;
    }
  }
  Value type = qe::Get(ctx, options, "type");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(type)) {
    out.type = NormalizeType(qe::ToUsvUtf8(ctx, type));
    if (qe::HasException(ctx)) return false;
  }
  return true;
}

template <typename T>
T* Allocate(Object* prototype) {
  T* blob = Heap::Allocate<T>();
  blob->initialize_prototype(prototype);
  return blob;
}

Value ConstructBlob(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Blob': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  // The arguments are converted in order: the parts, then the options.
  std::vector<Part> parts;
  if (!args.empty() && !qe::IsUndefined(args[0]) && !CollectParts(ctx, args[0], parts, "Blob")) return qe::Undefined();
  PropertyBag bag;
  if (args.size() > 1 && !ReadPropertyBag(ctx, args[1], false, bag, "Blob")) return qe::Undefined();
  std::string bytes = Join(parts, bag.nativeEndings);
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsBlob* blob = Allocate<JsBlob>(prototype ? prototype : BlobPrototype(ctx));
  blob->size = bytes.size();
  blob->data = std::make_shared<const std::string>(std::move(bytes));
  blob->type = bag.type;
  return qe::FromObject(blob);
}

Value ConstructFile(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'File': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  if (!RequireArguments(ctx, args, 2, "Failed to construct 'File'")) return qe::Undefined();
  std::vector<Part> parts;
  if (!CollectParts(ctx, args[0], parts, "File")) return qe::Undefined();
  const std::string name = qe::ToUsvUtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  PropertyBag bag;
  if (args.size() > 2 && !ReadPropertyBag(ctx, args[2], true, bag, "File")) return qe::Undefined();
  std::string bytes = Join(parts, bag.nativeEndings);
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsFile* file = Allocate<JsFile>(prototype ? prototype : FilePrototype(ctx));
  file->size = bytes.size();
  file->data = std::make_shared<const std::string>(std::move(bytes));
  file->type = bag.type;
  file->name = name;
  file->lastModified = bag.lastModified.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  return qe::FromObject(file);
}

Value GetSize(Context& ctx, Value t, qe::Args, Value) {
  JsBlob* self = This(ctx, t);
  return self ? Value(static_cast<double>(self->size)) : qe::Undefined();
}

Value GetType(Context& ctx, Value t, qe::Args, Value) {
  JsBlob* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->type) : qe::Undefined();
}

// A [Clamp] long long: NaN is 0, and what is out of range is the nearest end.
bool ReadClamped(Context& ctx, const Value& value, int64_t& out) {
  double number = 0;
  if (!value.to_number_checked(ctx, number)) return false;
  if (std::isnan(number)) number = 0;
  // [Clamp] rounds to the nearest integer, a half going to the even one, and clamps to the type.
  number = std::nearbyint(number);
  if (number >= 9223372036854775807.0) out = INT64_MAX;
  else if (number <= -9223372036854775808.0) out = INT64_MIN;
  else out = static_cast<int64_t>(number);
  return true;
}

Value Slice(Context& ctx, Value t, qe::Args args, Value) {
  JsBlob* self = This(ctx, t);
  if (!self) return qe::Undefined();
  const int64_t size = static_cast<int64_t>(self->size);
  int64_t start = 0, end = size;
  if (!args.empty() && !qe::IsUndefined(args[0]) && !ReadClamped(ctx, args[0], start)) return qe::Undefined();
  if (args.size() > 1 && !qe::IsUndefined(args[1]) && !ReadClamped(ctx, args[1], end)) return qe::Undefined();
  std::string type;
  if (args.size() > 2 && !qe::IsUndefined(args[2])) {
    type = NormalizeType(qe::ToUsvUtf8(ctx, args[2]));
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  const int64_t from = start < 0 ? std::max<int64_t>(size + start, 0) : std::min(start, size);
  const int64_t to = end < 0 ? std::max<int64_t>(size + end, 0) : std::min(end, size);

  JsBlob* slice = Allocate<JsBlob>(BlobPrototype(ctx));
  slice->data = self->data;
  slice->offset = self->offset + static_cast<size_t>(from);
  slice->size = static_cast<size_t>(std::max<int64_t>(to - from, 0));
  slice->type = type;
  return qe::FromObject(slice);
}

// The reading methods return promises, which are resolved already: the bytes are here.
Value Text(Context& ctx, Value t, qe::Args, Value) {
  JsBlob* self = This(ctx, t);
  if (!self) {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return RejectedPromise(ctx, error);
  }
  return ResolvedPromise(ctx, DecodeUtf8(ctx, self->Bytes()));
}

Value Stream(Context& ctx, Value t, qe::Args, Value) {
  JsBlob* self = This(ctx, t);
  return self ? BlobStream(ctx, self) : qe::Undefined();
}

Value ReadBytes(Context& ctx, JsBlob* self, bool asBuffer) {
  const std::string_view bytes = self->Bytes();
  Value array = qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
  if (qe::HasException(ctx)) {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return RejectedPromise(ctx, error);
  }
  return ResolvedPromise(ctx, asBuffer ? qe::Get(ctx, array, "buffer") : array);
}

template <bool AsBuffer>
Value Read(Context& ctx, Value t, qe::Args, Value) {
  JsBlob* self = This(ctx, t);
  if (!self) {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return RejectedPromise(ctx, error);
  }
  return ReadBytes(ctx, self, AsBuffer);
}

JsFile* ThisFile(Context& ctx, const Value& thisValue) {
  JsFile* self = DOMObject::Cast<JsFile>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value GetName(Context& ctx, Value t, qe::Args, Value) {
  JsFile* self = ThisFile(ctx, t);
  return self ? qe::FromUtf8(ctx, self->name) : qe::Undefined();
}

Value GetLastModified(Context& ctx, Value t, qe::Args, Value) {
  JsFile* self = ThisFile(ctx, t);
  return self ? Value(static_cast<double>(self->lastModified)) : qe::Undefined();
}

Value GetRelativePath(Context& ctx, Value t, qe::Args, Value) {
  return ThisFile(ctx, t) ? qe::FromUtf8(ctx, "") : qe::Undefined();
}

}  // namespace

JsBlob* NewBlob(Context& ctx, std::string bytes, std::string type) {
  JsBlob* blob = Allocate<JsBlob>(BlobPrototype(ctx));
  blob->size = bytes.size();
  blob->data = std::make_shared<const std::string>(std::move(bytes));
  blob->type = NormalizeType(type);
  return blob;
}

JsFile* NewFile(Context& ctx, std::string bytes, std::string name, std::string type, int64_t lastModified) {
  JsFile* file = Allocate<JsFile>(FilePrototype(ctx));
  file->size = bytes.size();
  file->data = std::make_shared<const std::string>(std::move(bytes));
  file->type = NormalizeType(type);
  file->name = std::move(name);
  file->lastModified = lastModified >= 0 ? lastModified : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return file;
}

void DefineBlobClasses(Context& ctx) {
  qe::ClassRef blob = qe::DefineClass(ctx, "Blob", ConstructBlob, 0);
  qe::SetRealmData(ctx, &g_blobPrototypeKey, blob.prototype);
  qe::DefineAccessor(blob.prototype, "size", GetSize, nullptr);
  qe::DefineAccessor(blob.prototype, "type", GetType, nullptr);
  qe::DefineMethod(blob.prototype, "slice", Slice, 0);
  qe::DefineMethod(blob.prototype, "text", Text, 0);
  qe::DefineMethod(blob.prototype, "stream", Stream, 0);
  qe::DefineMethod(blob.prototype, "arrayBuffer", Read<true>, 0);
  qe::DefineMethod(blob.prototype, "bytes", Read<false>, 0);
  qe::DefineGlobal(ctx, "Blob", blob.constructor);

  qe::ClassRef file = qe::DefineClass(ctx, "File", ConstructFile, 2, blob.prototype);
  qe::SetRealmData(ctx, &g_filePrototypeKey, file.prototype);
  qe::DefineAccessor(file.prototype, "name", GetName, nullptr);
  qe::DefineAccessor(file.prototype, "lastModified", GetLastModified, nullptr);
  qe::DefineAccessor(file.prototype, "webkitRelativePath", GetRelativePath, nullptr);
  qe::DefineGlobal(ctx, "File", file.constructor);
}

}  // namespace solar::web
