#include <algorithm>
#include <chrono>
#include <random>
#include <string>

#include "solar/url/UrlSearchParams.h"
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

char g_prototypeKey;
char g_iteratorPrototypeKey;

Object* Prototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_prototypeKey)); }
Object* IteratorPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_iteratorPrototypeKey)); }

enum class IterationKind { Entries, Keys, Values };

struct JsFormDataIterator : DOMObject {
  JsFormData* target = nullptr;
  IterationKind kind = IterationKind::Entries;
  size_t index = 0;
  void Visit(Quanta::Visitor& visitor) { visitor.Mark(target); }
};

JsFormData* This(Context& ctx, const Value& thisValue) {
  JsFormData* self = DOMObject::Cast<JsFormData>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

// The File an entry holds for a Blob it was given: the File itself, unless a filename renames it; a
// Blob that is no File becomes one, named "blob" if no filename came.
JsFile* AsFile(Context& ctx, const Value& value, const std::optional<std::string>& filename) {
  JsFile* existing = DOMObject::Cast<JsFile>(value);
  if (existing && !filename) return existing;
  JsBlob* blob = DOMObject::Cast<JsBlob>(value);
  return NewFile(ctx, std::string(blob->Bytes()), filename.value_or(existing ? existing->name : "blob"), blob->type, existing ? existing->lastModified : -1);
}

// The arguments of append and set: a name and either a string or a Blob, which may come with a filename.
// False, with an exception pending, if they are not.
bool ReadEntry(Context& ctx, qe::Args args, const char* method, FormDataEntry& out) {
  if (args.size() < 2) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'FormData': 2 arguments required, but only " + std::to_string(args.size()) + " present.");
    return false;
  }
  if (args.size() > 2 && !DOMObject::Cast<JsBlob>(args[1])) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'FormData': parameter 2 is not of type 'Blob'.");
    return false;
  }
  out.name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return false;
  if (DOMObject::Cast<JsBlob>(args[1])) {
    std::optional<std::string> filename;
    if (args.size() > 2 && !qe::IsUndefined(args[2])) {
      filename = qe::ToUsvUtf8(ctx, args[2]);
      if (qe::HasException(ctx)) return false;
    }
    out.file = AsFile(ctx, args[1], filename);
  } else {
    out.value = qe::ToUsvUtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return false;
  }
  return true;
}

Value ValueOf(Context& ctx, const FormDataEntry& entry) { return entry.file ? qe::FromObject(entry.file) : qe::FromUtf8(ctx, entry.value); }

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'FormData': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to construct 'FormData': there are no form elements to read from yet.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsFormData* form = NewFormData(ctx);
  if (prototype) form->initialize_prototype(prototype);
  return qe::FromObject(form);
}

Value Append(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  FormDataEntry entry;
  if (!ReadEntry(ctx, args, "append", entry)) return qe::Undefined();
  self->entries.push_back(entry);
  if (entry.file) self->NoteWrite(qe::FromObject(entry.file));
  return qe::Undefined();
}

Value Delete(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'delete' on 'FormData'")) return qe::Undefined();
  const std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::erase_if(self->entries, [&](const FormDataEntry& e) { return e.name == name; });
  return qe::Undefined();
}

Value Get(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'get' on 'FormData'")) return qe::Undefined();
  const std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  for (const FormDataEntry& entry : self->entries) {
    if (entry.name == name) return ValueOf(ctx, entry);
  }
  return qe::Null();
}

Value GetAll(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'getAll' on 'FormData'")) return qe::Undefined();
  const std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value array = qe::NewArray(ctx);
  for (const FormDataEntry& entry : self->entries) {
    if (entry.name == name) qe::ArrayPush(ctx, array, ValueOf(ctx, entry));
  }
  return array;
}

Value Has(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'has' on 'FormData'")) return qe::Undefined();
  const std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromBool(std::any_of(self->entries.begin(), self->entries.end(), [&](const FormDataEntry& e) { return e.name == name; }));
}

Value Set(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  FormDataEntry entry;
  if (!ReadEntry(ctx, args, "set", entry)) return qe::Undefined();
  // The first entry of that name is replaced where it stands, and the others go.
  bool replaced = false;
  for (auto it = self->entries.begin(); it != self->entries.end();) {
    if (it->name != entry.name) {
      ++it;
    } else if (!replaced) {
      *it = entry;
      replaced = true;
      ++it;
    } else {
      it = self->entries.erase(it);
    }
  }
  if (!replaced) self->entries.push_back(entry);
  if (entry.file) self->NoteWrite(qe::FromObject(entry.file));
  return qe::Undefined();
}

Value ForEach(Context& ctx, Value t, qe::Args args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'forEach' on 'FormData'")) return qe::Undefined();
  if (!qe::IsCallable(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to execute 'forEach' on 'FormData': parameter 1 is not of type 'Function'.");
    return qe::Undefined();
  }
  Value thisArg = args.size() > 1 ? args[1] : qe::Undefined();
  for (size_t i = 0; i < self->entries.size(); ++i) {
    const FormDataEntry entry = self->entries[i];
    Value callbackArgs[3] = {ValueOf(ctx, entry), qe::FromUtf8(ctx, entry.name), t};
    qe::Call(ctx, args[0], thisArg, qe::Args(callbackArgs, 3));
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::Undefined();
}

template <IterationKind Kind>
Value MakeIterator(Context& ctx, Value t, qe::Args, Value) {
  JsFormData* self = This(ctx, t);
  if (!self) return qe::Undefined();
  JsFormDataIterator* iterator = Heap::Allocate<JsFormDataIterator>();
  iterator->target = self;
  iterator->kind = Kind;
  iterator->initialize_prototype(self->iteratorPrototype);
  return qe::FromObject(iterator);
}

Value IteratorNext(Context& ctx, Value t, qe::Args, Value) {
  JsFormDataIterator* iterator = DOMObject::Cast<JsFormDataIterator>(t);
  if (!iterator) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  if (iterator->index >= iterator->target->entries.size()) return qe::MakeIterResult(ctx, qe::Undefined(), true);
  const FormDataEntry& entry = iterator->target->entries[iterator->index++];
  switch (iterator->kind) {
    case IterationKind::Keys:
      return qe::MakeIterResult(ctx, qe::FromUtf8(ctx, entry.name), false);
    case IterationKind::Values:
      return qe::MakeIterResult(ctx, ValueOf(ctx, entry), false);
    case IterationKind::Entries: {
      Value pair = qe::NewArray(ctx);
      qe::ArrayPush(ctx, pair, qe::FromUtf8(ctx, entry.name));
      qe::ArrayPush(ctx, pair, ValueOf(ctx, entry));
      return qe::MakeIterResult(ctx, pair, false);
    }
  }
  return qe::Undefined();
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

// ---- multipart/form-data ----

// Newlines in a name or string value are CRLF in the encoding.
std::string CrlfNewlines(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      out += "\r\n";
    } else if (text[i] == '\n') {
      out += "\r\n";
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

// A name or filename inside the quotes of a header: a quote, CR and LF can only be said in percent.
std::string EscapeForHeader(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '"') out += "%22";
    else if (c == '\r') out += "%0D";
    else if (c == '\n') out += "%0A";
    else out.push_back(c);
  }
  return out;
}

std::string UnescapeFromHeader(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const std::string code = text.substr(i + 1, 2);
      if (code == "22") { out.push_back('"'); i += 2; continue; }
      if (code == "0D" || code == "0d") { out.push_back('\r'); i += 2; continue; }
      if (code == "0A" || code == "0a") { out.push_back('\n'); i += 2; continue; }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

// The value of a parameter of a header ("boundary" of a Content-Type, "name" of a Content-Disposition),
// quoted or not; none if it has none.
std::optional<std::string> Parameter(std::string_view header, std::string_view wanted) {
  size_t pos = header.find(';');
  while (pos != std::string_view::npos && pos < header.size()) {
    ++pos;
    const size_t next = [&] {
      // A parameter ends at the next semicolon that is not inside quotes.
      bool quoted = false;
      for (size_t i = pos; i < header.size(); ++i) {
        if (header[i] == '"') quoted = !quoted;
        if (header[i] == ';' && !quoted) return i;
      }
      return std::string_view::npos;
    }();
    const std::string_view parameter = header.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    const size_t equals = parameter.find('=');
    if (equals != std::string_view::npos && EqualsIgnoreCase(Trim(parameter.substr(0, equals)), wanted)) {
      std::string_view value = Trim(parameter.substr(equals + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      return std::string(value);
    }
    pos = next;
  }
  return std::nullopt;
}

}  // namespace

JsFormData* NewFormData(Context& ctx) {
  JsFormData* form = Heap::Allocate<JsFormData>();
  form->iteratorPrototype = IteratorPrototype(ctx);
  form->initialize_prototype(Prototype(ctx));
  return form;
}

void EncodeMultipart(const JsFormData& form, std::string& body, std::string& contentType) {
  static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device random;
  std::string boundary = "----SolarFormBoundary";
  for (int i = 0; i < 16; ++i) boundary.push_back(kAlphabet[random() % (sizeof(kAlphabet) - 1)]);

  body.clear();
  for (const FormDataEntry& entry : form.entries) {
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + EscapeForHeader(CrlfNewlines(entry.name)) + "\"";
    if (entry.file) {
      body += "; filename=\"" + EscapeForHeader(entry.file->name) + "\"\r\nContent-Type: " + (entry.file->type.empty() ? "application/octet-stream" : entry.file->type) + "\r\n\r\n";
      body.append(entry.file->Bytes());
    } else {
      body += "\r\n\r\n" + CrlfNewlines(entry.value);
    }
    body += "\r\n";
  }
  body += "--" + boundary + "--\r\n";
  contentType = "multipart/form-data; boundary=" + boundary;
}

bool ParseFormData(Context& ctx, std::string_view body, std::string_view contentType, JsFormData* into) {
  const size_t semicolon = contentType.find(';');
  std::string essence(Trim(contentType.substr(0, semicolon)));
  for (char& c : essence) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c;

  if (essence == "application/x-www-form-urlencoded") {
    // UTF-8 decoded without dropping a byte order mark: one at the start is part of the first name.
    const url::UrlSearchParams params{std::string(body)};
    for (const auto& pair : params.List()) into->entries.push_back({pair.first, pair.second, nullptr});
    return true;
  }
  if (essence != "multipart/form-data") return false;

  const std::optional<std::string> boundary = Parameter(contentType, "boundary");
  if (!boundary || boundary->empty()) return false;
  const std::string delimiter = "--" + *boundary;

  size_t pos = body.find(delimiter);
  if (pos == std::string_view::npos) return false;
  pos += delimiter.size();
  while (true) {
    if (body.substr(pos, 2) == "--") return true;  // the closing delimiter
    const size_t lineEnd = body.find("\r\n", pos);  // the rest of the delimiter's line
    if (lineEnd == std::string_view::npos) return false;
    pos = lineEnd + 2;

    std::string_view headers;
    size_t contentStart;
    if (body.substr(pos, 2) == "\r\n") {  // a part with no headers
      contentStart = pos + 2;
    } else {
      const size_t headersEnd = body.find("\r\n\r\n", pos);
      if (headersEnd == std::string_view::npos) return false;
      headers = body.substr(pos, headersEnd - pos);
      contentStart = headersEnd + 4;
    }
    const size_t next = body.find("\r\n" + delimiter, contentStart);
    if (next == std::string_view::npos) return false;
    const std::string_view content = body.substr(contentStart, next - contentStart);

    std::optional<std::string> name, filename, type;
    size_t line = 0;
    while (line < headers.size()) {
      size_t end = headers.find("\r\n", line);
      if (end == std::string_view::npos) end = headers.size();
      const std::string_view header = headers.substr(line, end - line);
      line = end + 2;
      const size_t colon = header.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string_view headerName = Trim(header.substr(0, colon));
      const std::string_view value = Trim(header.substr(colon + 1));
      if (EqualsIgnoreCase(headerName, "content-disposition")) {
        if (!value.starts_with("form-data")) continue;
        name = Parameter(value, "name");
        filename = Parameter(value, "filename");
      } else if (EqualsIgnoreCase(headerName, "content-type")) {
        type = std::string(value);
      }
    }
    if (name) {
      if (filename) {
        into->entries.push_back({UnescapeFromHeader(*name), "", NewFile(ctx, std::string(content), UnescapeFromHeader(*filename), type.value_or("text/plain"), -1)});
        into->NoteWrite(qe::FromObject(into->entries.back().file));
      } else {
        into->entries.push_back({UnescapeFromHeader(*name), std::string(content), nullptr});
      }
    }
    pos = next + 2 + delimiter.size();
  }
}

void DefineFormDataClass(Context& ctx) {
  qe::ClassRef iterator = qe::DefineClass(ctx, "FormData Iterator", IllegalConstructor, 0, qe::GetIteratorPrototype(ctx));
  qe::SetRealmData(ctx, &g_iteratorPrototypeKey, iterator.prototype);
  qe::DefineMethod(iterator.prototype, "next", IteratorNext, 0);
  qe::DefineToStringTag(iterator.prototype, "FormData Iterator");

  qe::ClassRef form = qe::DefineClass(ctx, "FormData", Construct, 0);
  qe::SetRealmData(ctx, &g_prototypeKey, form.prototype);
  qe::DefineMethod(form.prototype, "append", Append, 2);
  qe::DefineMethod(form.prototype, "delete", Delete, 1);
  qe::DefineMethod(form.prototype, "get", Get, 1);
  qe::DefineMethod(form.prototype, "getAll", GetAll, 1);
  qe::DefineMethod(form.prototype, "has", Has, 1);
  qe::DefineMethod(form.prototype, "set", Set, 2);
  qe::DefineMethod(form.prototype, "forEach", ForEach, 1);
  qe::DefineMethod(form.prototype, "entries", MakeIterator<IterationKind::Entries>, 0);
  qe::DefineMethod(form.prototype, "keys", MakeIterator<IterationKind::Keys>, 0);
  qe::DefineMethod(form.prototype, "values", MakeIterator<IterationKind::Values>, 0);
  qe::DefineGlobal(ctx, "FormData", form.constructor);
}

}  // namespace solar::web
