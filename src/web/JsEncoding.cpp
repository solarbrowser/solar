#include <algorithm>
#include <string>

#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/UrlBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_encoderPrototypeKey;
char g_decoderPrototypeKey;

Object* EncoderPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_encoderPrototypeKey)); }
Object* DecoderPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_decoderPrototypeKey)); }

void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

enum class Encoding { Utf8, Utf16Le, Utf16Be, Windows1252 };

const char* NameOf(Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8: return "utf-8";
    case Encoding::Utf16Le: return "utf-16le";
    case Encoding::Utf16Be: return "utf-16be";
    case Encoding::Windows1252: return "windows-1252";
  }
  return "utf-8";
}

// The Encoding Standard's labels for the encodings this build can decode.
std::optional<Encoding> EncodingForLabel(std::string label) {
  size_t start = 0, end = label.size();
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; };
  while (start < end && isSpace(label[start])) ++start;
  while (end > start && isSpace(label[end - 1])) --end;
  label = label.substr(start, end - start);
  for (char& c : label) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c;
  for (const char* name : {"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"}) {
    if (label == name) return Encoding::Utf8;
  }
  for (const char* name : {"csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff", "utf-16", "utf-16le"}) {
    if (label == name) return Encoding::Utf16Le;
  }
  for (const char* name : {"unicodefffe", "utf-16be"}) {
    if (label == name) return Encoding::Utf16Be;
  }
  for (const char* name : {"ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819", "iso-8859-1", "iso-ir-100", "iso8859-1", "iso88591",
                           "iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii", "windows-1252", "x-cp1252"}) {
    if (label == name) return Encoding::Windows1252;
  }
  return std::nullopt;
}

constexpr char32_t kWindows1252[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                       0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

struct JsTextEncoder : DOMObject {
  void Visit(Quanta::Visitor&) {}
};

// A TextDecoder and the state of a decoding that goes on across calls with stream: true.
struct JsTextDecoder : DOMObject {
  Encoding encoding = Encoding::Utf8;
  bool fatal = false;
  bool ignoreBom = false;

  // UTF-8
  char32_t codePoint = 0;
  int needed = 0, seen = 0;
  uint8_t lower = 0x80, upper = 0xBF;
  // UTF-16
  int leadByte = -1;
  int leadSurrogate = -1;
  // All
  bool started = false;  // something has been written, so a byte order mark is no longer one

  void Visit(Quanta::Visitor&) {}
  void Reset() {
    codePoint = 0;
    needed = seen = 0;
    lower = 0x80;
    upper = 0xBF;
    leadByte = leadSurrogate = -1;
    started = false;
  }
};

// Emits a code point, dropping a byte order mark at the start unless it is to be kept.
void Emit(JsTextDecoder& d, char32_t cp, std::string& out) {
  if (!d.started) {
    d.started = true;
    if (cp == 0xFEFF && !d.ignoreBom) return;
  }
  AppendUtf8(out, cp);
}

// Each reports whether the bytes were valid, and with fatal false carries on after an error.
bool DecodeUtf8(JsTextDecoder& d, std::span<const uint8_t> bytes, bool stream, std::string& out) {
  bool ok = true;
  for (size_t i = 0; i < bytes.size();) {
    const uint8_t b = bytes[i];
    if (d.needed == 0) {
      ++i;
      if (b <= 0x7F) {
        Emit(d, b, out);
      } else if (b >= 0xC2 && b <= 0xDF) {
        d.needed = 1;
        d.codePoint = b & 0x1F;
      } else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) d.lower = 0xA0;
        if (b == 0xED) d.upper = 0x9F;
        d.needed = 2;
        d.codePoint = b & 0xF;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) d.lower = 0x90;
        if (b == 0xF4) d.upper = 0x8F;
        d.needed = 3;
        d.codePoint = b & 0x7;
      } else {
        ok = false;
        if (d.fatal) return false;
        Emit(d, 0xFFFD, out);
      }
      continue;
    }
    if (b < d.lower || b > d.upper) {
      d.codePoint = 0;
      d.needed = d.seen = 0;
      d.lower = 0x80;
      d.upper = 0xBF;
      ok = false;
      if (d.fatal) return false;
      Emit(d, 0xFFFD, out);
      continue;  // the byte is looked at again, as the start of something
    }
    ++i;
    d.lower = 0x80;
    d.upper = 0xBF;
    d.codePoint = (d.codePoint << 6) | (b & 0x3F);
    if (++d.seen == d.needed) {
      Emit(d, d.codePoint, out);
      d.codePoint = 0;
      d.needed = d.seen = 0;
    }
  }
  if (!stream && d.needed != 0) {
    d.codePoint = 0;
    d.needed = d.seen = 0;
    d.lower = 0x80;
    d.upper = 0xBF;
    ok = false;
    if (d.fatal) return false;
    Emit(d, 0xFFFD, out);
  }
  return ok;
}

bool DecodeUtf16(JsTextDecoder& d, std::span<const uint8_t> bytes, bool stream, std::string& out) {
  bool ok = true;
  const bool bigEndian = d.encoding == Encoding::Utf16Be;
  const auto handle = [&](int unit) {
    // Returns false for an error in fatal mode; a unit left over is dealt with by the caller.
    if (d.leadSurrogate != -1) {
      const int lead = d.leadSurrogate;
      d.leadSurrogate = -1;
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        Emit(d, 0x10000 + ((lead - 0xD800) << 10) + (unit - 0xDC00), out);
        return true;
      }
      ok = false;
      if (d.fatal) return false;
      Emit(d, 0xFFFD, out);  // the lead had no trail; the unit is looked at again below
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      d.leadSurrogate = unit;
    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
      ok = false;
      if (d.fatal) return false;
      Emit(d, 0xFFFD, out);
    } else {
      Emit(d, static_cast<char32_t>(unit), out);
    }
    return true;
  };
  for (uint8_t b : bytes) {
    if (d.leadByte == -1) {
      d.leadByte = b;
      continue;
    }
    const int unit = bigEndian ? (d.leadByte << 8) | b : (b << 8) | d.leadByte;
    d.leadByte = -1;
    if (!handle(unit)) return false;
  }
  if (!stream) {
    if (d.leadByte != -1 || d.leadSurrogate != -1) {
      d.leadByte = d.leadSurrogate = -1;
      ok = false;
      if (d.fatal) return false;
      Emit(d, 0xFFFD, out);
    }
  }
  return ok;
}

void DecodeWindows1252(JsTextDecoder& d, std::span<const uint8_t> bytes, std::string& out) {
  for (uint8_t b : bytes) Emit(d, b >= 0x80 && b <= 0x9F ? kWindows1252[b - 0x80] : b, out);
}

// ---- TextEncoder ----

Value ConstructEncoder(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'TextEncoder': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsTextEncoder* encoder = Heap::Allocate<JsTextEncoder>();
  encoder->initialize_prototype(prototype ? prototype : EncoderPrototype(ctx));
  return qe::FromObject(encoder);
}

bool ThisEncoder(Context& ctx, const Value& thisValue) {
  if (DOMObject::Cast<JsTextEncoder>(thisValue)) return true;
  qe::ThrowTypeError(ctx, "Illegal invocation");
  return false;
}

Value GetEncoderEncoding(Context& ctx, Value t, qe::Args, Value) { return ThisEncoder(ctx, t) ? qe::FromUtf8(ctx, "utf-8") : qe::Undefined(); }

Value Encode(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisEncoder(ctx, t)) return qe::Undefined();
  std::string text;
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    text = qe::ToUsvUtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

// The part of encodeInto that needs the bytes: how much of the string was read, in UTF-16 code units, and
// how many bytes written. The method itself is script, in InstallEncodingApis, so that it can return the
// dictionary the standard has it return.
Value EncodeIntoNative(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) {
    qe::ThrowTypeError(ctx, "Failed to execute 'encodeInto' on 'TextEncoder': 2 arguments required.");
    return qe::Undefined();
  }
  const std::string text = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::span<const uint8_t>> destination = qe::BytesOf(args[1]);
  if (!destination) {
    qe::ThrowTypeError(ctx, "Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
    return qe::Undefined();
  }
  // BytesOf hands out what it points into as const; the destination is meant to be written.
  uint8_t* target = const_cast<uint8_t*>(destination->data());
  size_t read = 0, written = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (written + length > destination->size()) break;
    std::copy_n(text.data() + i, length, reinterpret_cast<char*>(target + written));
    written += length;
    i += length;
    read += length == 4 ? 2 : 1;
  }
  Value result = qe::NewArray(ctx);
  qe::ArrayPush(ctx, result, Value(static_cast<double>(read)));
  qe::ArrayPush(ctx, result, Value(static_cast<double>(written)));
  return result;
}

// ---- TextDecoder ----

Value ConstructDecoder(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'TextDecoder': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  std::string label = "utf-8";
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    label = qe::ToUsvUtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  bool fatal = false, ignoreBom = false;
  if (args.size() > 1 && !qe::IsUndefined(args[1])) {
    if (!qe::IsObject(args[1]) && !qe::IsNull(args[1])) {
      qe::ThrowTypeError(ctx, "Failed to construct 'TextDecoder': The provided value is not of type 'TextDecoderOptions'.");
      return qe::Undefined();
    }
    if (qe::IsObject(args[1])) {
      Value f = qe::Get(ctx, args[1], "fatal");
      if (qe::HasException(ctx)) return qe::Undefined();
      fatal = f.to_boolean();
      Value i = qe::Get(ctx, args[1], "ignoreBOM");
      if (qe::HasException(ctx)) return qe::Undefined();
      ignoreBom = i.to_boolean();
    }
  }
  const std::optional<Encoding> encoding = EncodingForLabel(label);
  if (!encoding) {
    ctx.throw_range_error("Failed to construct 'TextDecoder': The encoding label provided ('" + label + "') is invalid or not supported.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsTextDecoder* decoder = Heap::Allocate<JsTextDecoder>();
  decoder->initialize_prototype(prototype ? prototype : DecoderPrototype(ctx));
  decoder->encoding = *encoding;
  decoder->fatal = fatal;
  decoder->ignoreBom = ignoreBom;
  return qe::FromObject(decoder);
}

JsTextDecoder* ThisDecoder(Context& ctx, const Value& thisValue) {
  JsTextDecoder* self = DOMObject::Cast<JsTextDecoder>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value GetDecoderEncoding(Context& ctx, Value t, qe::Args, Value) {
  JsTextDecoder* self = ThisDecoder(ctx, t);
  return self ? qe::FromUtf8(ctx, NameOf(self->encoding)) : qe::Undefined();
}

Value GetFatal(Context& ctx, Value t, qe::Args, Value) {
  JsTextDecoder* self = ThisDecoder(ctx, t);
  return self ? qe::FromBool(self->fatal) : qe::Undefined();
}

Value GetIgnoreBom(Context& ctx, Value t, qe::Args, Value) {
  JsTextDecoder* self = ThisDecoder(ctx, t);
  return self ? qe::FromBool(self->ignoreBom) : qe::Undefined();
}

Value Decode(Context& ctx, Value t, qe::Args args, Value) {
  JsTextDecoder* self = ThisDecoder(ctx, t);
  if (!self) return qe::Undefined();
  // The input is converted first, the options next, and the bytes are read after both: an option's
  // getter that detaches or changes the buffer is seen.
  const bool hasInput = !args.empty() && !qe::IsUndefined(args[0]);
  if (hasInput && !qe::BytesOf(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to execute 'decode' on 'TextDecoder': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
    return qe::Undefined();
  }
  bool stream = false;
  if (args.size() > 1 && !qe::IsUndefined(args[1]) && !qe::IsNull(args[1])) {
    if (!qe::IsObject(args[1])) {
      qe::ThrowTypeError(ctx, "Failed to execute 'decode' on 'TextDecoder': The provided value is not of type 'TextDecodeOptions'.");
      return qe::Undefined();
    }
    Value option = qe::Get(ctx, args[1], "stream");
    if (qe::HasException(ctx)) return qe::Undefined();
    stream = option.to_boolean();
  }
  std::string input;
  if (hasInput) {
    if (std::optional<std::span<const uint8_t>> view = qe::BytesOf(args[0])) input.assign(reinterpret_cast<const char*>(view->data()), view->size());
  }

  const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  std::string out;
  bool ok = true;
  switch (self->encoding) {
    case Encoding::Utf8: ok = DecodeUtf8(*self, bytes, stream, out); break;
    case Encoding::Utf16Le:
    case Encoding::Utf16Be: ok = DecodeUtf16(*self, bytes, stream, out); break;
    case Encoding::Windows1252: DecodeWindows1252(*self, bytes, out); break;
  }
  if (!ok || !stream) self->Reset();
  if (!ok && self->fatal) {  // without fatal, what was wrong has been replaced
    qe::ThrowTypeError(ctx, std::string("Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid for encoding ") + NameOf(self->encoding));
    return qe::Undefined();
  }
  return qe::FromUtf8(ctx, out);
}

}  // namespace

void DefineEncodingClasses(Context& ctx) {
  qe::ClassRef encoder = qe::DefineClass(ctx, "TextEncoder", ConstructEncoder, 0);
  qe::SetRealmData(ctx, &g_encoderPrototypeKey, encoder.prototype);
  qe::DefineAccessor(encoder.prototype, "encoding", GetEncoderEncoding, nullptr);
  qe::DefineMethod(encoder.prototype, "encode", Encode, 0);
  qe::DefineGlobal(ctx, "TextEncoder", encoder.constructor);
  qe::DefineGlobalFunction(ctx, "__solarEncodeInto", EncodeIntoNative, 2);

  qe::ClassRef decoder = qe::DefineClass(ctx, "TextDecoder", ConstructDecoder, 0);
  qe::SetRealmData(ctx, &g_decoderPrototypeKey, decoder.prototype);
  qe::DefineAccessor(decoder.prototype, "encoding", GetDecoderEncoding, nullptr);
  qe::DefineAccessor(decoder.prototype, "fatal", GetFatal, nullptr);
  qe::DefineAccessor(decoder.prototype, "ignoreBOM", GetIgnoreBom, nullptr);
  qe::DefineMethod(decoder.prototype, "decode", Decode, 0);
  qe::DefineGlobal(ctx, "TextDecoder", decoder.constructor);
}

}  // namespace solar::web
