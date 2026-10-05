#include "solar/html/Modules.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "solar/html/Frames.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

constexpr const char kInlineMarker[] = "#solar-inline-";

// ---- The import map ----

using SpecifierMap = std::vector<std::pair<std::string, std::optional<std::string>>>;  // address: none for null

struct ImportMap {
  SpecifierMap imports;
  std::vector<std::pair<std::string, SpecifierMap>> scopes;
};

std::map<qe::Realm*, dom::Document*>& Registry() {
  static std::map<qe::Realm*, dom::Document*> registry;
  return registry;
}

bool IsUrlLikeRelative(const std::string& specifier) { return specifier.starts_with("/") || specifier.starts_with("./") || specifier.starts_with("../"); }

// "parse a URL-like import specifier": a relative reference or an absolute URL, resolved against `base`.
std::optional<url::Url> ParseUrlLike(const std::string& specifier, const std::optional<url::Url>& base) {
  if (IsUrlLikeRelative(specifier)) return url::Parse(specifier, base ? &*base : nullptr);
  return url::Parse(specifier);
}

SpecifierMap NormalizeSpecifierMap(const std::vector<std::pair<std::string, std::optional<std::string>>>& entries, const std::optional<url::Url>& base) {
  SpecifierMap out;
  for (const auto& [key, value] : entries) {
    if (key.empty()) continue;
    std::string normalizedKey = key;
    if (const auto asUrl = ParseUrlLike(key, base)) normalizedKey = url::Serialize(*asUrl);
    if (!value) {
      out.emplace_back(normalizedKey, std::nullopt);
      continue;
    }
    const auto address = ParseUrlLike(*value, base);
    if (!address) {
      out.emplace_back(normalizedKey, std::nullopt);
      continue;
    }
    // A key with a trailing slash maps to an address with one.
    const std::string serialized = url::Serialize(*address);
    if (normalizedKey.ends_with("/") && !serialized.ends_with("/")) {
      out.emplace_back(normalizedKey, std::nullopt);
      continue;
    }
    out.emplace_back(normalizedKey, serialized);
  }
  // Longer keys first, so that the most specific prefix wins.
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  return out;
}

// "resolve an imports match": the address `normalized` maps to in `map`, a null address meaning it is blocked.
// The result is nothing for no match and an empty string for a block.
std::optional<std::string> ResolveImportsMatch(const std::string& normalized, bool asUrlSpecial, const SpecifierMap& map, std::string& error) {
  for (const auto& [key, address] : map) {
    if (key == normalized) {
      if (!address) {
        error = "The specifier \"" + normalized + "\" was a bare specifier, but was blocked by the import map.";
        return std::string();
      }
      return *address;
    }
    if (key.ends_with("/") && normalized.starts_with(key) && (!asUrlSpecial || true)) {
      if (!address) {
        error = "The specifier \"" + normalized + "\" was blocked by the import map.";
        return std::string();
      }
      const std::string afterPrefix = normalized.substr(key.size());
      const auto base = url::Parse(*address);
      const auto resolved = url::Parse(afterPrefix, base ? &*base : nullptr);
      if (!resolved || !url::Serialize(*resolved).starts_with(*address)) {
        error = "The specifier \"" + normalized + "\" backtracks above its prefix.";
        return std::string();
      }
      return url::Serialize(*resolved);
    }
  }
  return std::nullopt;
}

dom::Document* DocumentOf(qe::Realm* realm) {
  const auto found = Registry().find(realm);
  return found == Registry().end() ? nullptr : found->second;
}

ImportMap* ImportMapOf(dom::Document* document, bool create) {
  if (!document) return nullptr;
  if (!document->importMap && create) document->importMap = std::make_shared<ImportMap>();
  return static_cast<ImportMap*>(document->importMap.get());
}

bool Resolve(qe::Realm* realm, const std::string& specifier, const std::string& referrer, std::string& resolved, std::string& error) {
  dom::Document* document = DocumentOf(realm);
  std::string base = VisibleModuleUrl(referrer);
  if (base.empty() && document) base = DocumentBaseUrl(document);
  const std::optional<url::Url> baseUrl = url::Parse(base);
  const std::optional<url::Url> asUrl = ParseUrlLike(specifier, baseUrl);
  const std::string normalized = asUrl ? url::Serialize(*asUrl) : specifier;
  if (const ImportMap* map = ImportMapOf(document, false)) {
    // The scopes the referrer is in, the most specific first, then the map for the whole page.
    std::vector<const std::pair<std::string, SpecifierMap>*> scopes;
    for (const auto& scope : map->scopes) {
      if (scope.first == base || (scope.first.ends_with("/") && base.starts_with(scope.first))) scopes.push_back(&scope);
    }
    std::stable_sort(scopes.begin(), scopes.end(), [](const auto* a, const auto* b) { return a->first.size() > b->first.size(); });
    for (const auto* scope : scopes) {
      const std::optional<std::string> match = ResolveImportsMatch(normalized, asUrl && url::IsSpecialScheme(asUrl->scheme), scope->second, error);
      if (match) {
        if (match->empty()) return false;
        resolved = *match;
        return true;
      }
    }
    const std::optional<std::string> match = ResolveImportsMatch(normalized, asUrl && url::IsSpecialScheme(asUrl->scheme), map->imports, error);
    if (match) {
      if (match->empty()) return false;
      resolved = *match;
      return true;
    }
  }
  if (asUrl) {
    resolved = normalized;
    return true;
  }
  error = "Failed to resolve module specifier \"" + specifier + "\". Relative references must start with either \"/\", \"./\", or \"../\".";
  return false;
}

// ---- Fetching ----

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// What a data: URL holds, and its media type.
std::optional<std::pair<std::string, std::string>> DecodeDataUrl(const std::string& address) {
  const size_t comma = address.find(',');
  if (comma == std::string::npos) return std::nullopt;
  std::string header = address.substr(5, comma - 5);
  std::string body = address.substr(comma + 1);
  const size_t hash = body.find('#');
  if (hash != std::string::npos) body.resize(hash);
  std::string decoded;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '%' && i + 2 < body.size() + 0 && std::isxdigit(static_cast<unsigned char>(body[i + 1])) && std::isxdigit(static_cast<unsigned char>(body[i + 2]))) {
      decoded += static_cast<char>(std::stoi(body.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      decoded += body[i];
    }
  }
  bool base64 = false;
  const size_t semicolon = header.rfind(';');
  if (semicolon != std::string::npos && header.substr(semicolon + 1) == "base64") {
    base64 = true;
    header.resize(semicolon);
  }
  if (base64) {
    std::string bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : decoded) {
      const int value = Base64Value(c);
      if (value < 0) continue;
      buffer = (buffer << 6) | static_cast<uint32_t>(value);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes += static_cast<char>((buffer >> bits) & 0xFF);
      }
    }
    decoded = std::move(bytes);
  }
  return std::make_pair(decoded, header.empty() ? "text/plain" : header);
}

void Fetch(qe::Realm*, const std::string& address, const std::string& type, std::function<void(qe::ModuleSource)> done) {
  std::optional<std::string> text;
  if (address.starts_with("data:")) {
    if (auto data = DecodeDataUrl(address)) text = std::move(data->first);
  } else {
    text = LoadResource(address);
  }
  if (!text) {
    done(qe::ModuleSource::Failure("Failed to fetch module " + VisibleModuleUrl(address)));
    return;
  }
  if (type.empty()) done(qe::ModuleSource::Script(std::move(*text)));
  else if (type == "json") done(qe::ModuleSource::Json(std::move(*text)));
  else done(qe::ModuleSource::Failure("Unsupported module type \"" + type + "\""));
}

// ---- Parsing an import map ----

bool ReadStringMap(Context& ctx, const Value& object, std::vector<std::pair<std::string, std::optional<std::string>>>& out, std::string& error) {
  if (!qe::IsObject(object) || qe::IsCallable(object)) {
    error = "An import map's specifier map must be an object.";
    return false;
  }
  for (const std::string& key : qe::OwnKeys(ctx, object)) {
    Value value = qe::Get(ctx, object, key);
    if (qe::HasException(ctx)) {
      ctx.clear_exception();
      continue;
    }
    if (value.is_string()) out.emplace_back(key, qe::ToWtf8(ctx, value));
    else out.emplace_back(key, std::nullopt);
  }
  return true;
}

}  // namespace

std::string InlineModuleUrl(dom::Document* document) {
  static uint64_t counter = 0;
  return DocumentBaseUrl(document) + kInlineMarker + std::to_string(++counter);
}

std::string VisibleModuleUrl(const std::string& url) {
  const size_t at = url.find(kInlineMarker);
  return at == std::string::npos ? url : url.substr(0, at);
}

void RegisterRealmDocument(qe::Realm* realm, dom::Document* document) { Registry()[realm] = document; }
void ForgetRealm(qe::Realm* realm) { Registry().erase(realm); }

bool RegisterImportMap(qe::Realm& realm, dom::Document* document, const std::string& json, std::string& error) {
  Context& ctx = realm.GetContext();
  Value global = qe::FromObject(ctx.get_global_object());
  Value jsonObject = qe::Get(ctx, global, "JSON");
  Value parse = qe::Get(ctx, jsonObject, "parse");
  Value text = qe::FromWtf8(ctx, json);
  Value parsed = qe::Call(ctx, parse, jsonObject, qe::Args(&text, 1));
  if (qe::HasException(ctx)) {
    error = qe::InspectError(ctx, ctx.get_exception()).message;
    ctx.clear_exception();
    return false;
  }
  if (!qe::IsObject(parsed) || qe::IsCallable(parsed)) {
    error = "An import map must be a JSON object.";
    return false;
  }
  const std::optional<url::Url> base = url::Parse(DocumentBaseUrl(document));
  ImportMap incoming;
  Value imports = qe::Get(ctx, parsed, "imports");
  if (!qe::IsUndefined(imports)) {
    std::vector<std::pair<std::string, std::optional<std::string>>> entries;
    if (!ReadStringMap(ctx, imports, entries, error)) return false;
    incoming.imports = NormalizeSpecifierMap(entries, base);
  }
  Value scopes = qe::Get(ctx, parsed, "scopes");
  if (!qe::IsUndefined(scopes)) {
    if (!qe::IsObject(scopes)) {
      error = "An import map's scopes must be an object.";
      return false;
    }
    for (const std::string& prefix : qe::OwnKeys(ctx, scopes)) {
      const auto prefixUrl = url::Parse(prefix, base ? &*base : nullptr);
      if (!prefixUrl) continue;
      std::vector<std::pair<std::string, std::optional<std::string>>> entries;
      if (!ReadStringMap(ctx, qe::Get(ctx, scopes, prefix), entries, error)) return false;
      incoming.scopes.emplace_back(url::Serialize(*prefixUrl), NormalizeSpecifierMap(entries, base));
    }
  }
  // Later import maps add to the ones before: what is mapped already stays as it is.
  ImportMap* map = ImportMapOf(document, true);
  for (auto& entry : incoming.imports) {
    const bool present = std::any_of(map->imports.begin(), map->imports.end(), [&](const auto& e) { return e.first == entry.first; });
    if (!present) map->imports.push_back(std::move(entry));
  }
  std::stable_sort(map->imports.begin(), map->imports.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  for (auto& scope : incoming.scopes) map->scopes.push_back(std::move(scope));
  return true;
}

qe::ModuleHooks MakeModuleHooks() {
  qe::ModuleHooks hooks;
  hooks.resolve = Resolve;
  hooks.fetch = Fetch;
  hooks.initImportMeta = [](qe::Realm* realm, const Value& meta, const std::string& url) {
    Context& ctx = realm->GetContext();
    qe::Set(ctx, meta, "url", qe::FromWtf8(ctx, VisibleModuleUrl(url)));
    // import.meta.resolve(specifier): what the import would be, or a TypeError.
    Value resolve = qe::NewFunction(ctx, "resolve", 1, [realm, url](Context& c, Value, qe::Args args, Value) {
      if (args.empty()) {
        qe::ThrowTypeError(c, "Failed to execute 'resolve' on 'import.meta': 1 argument required, but only 0 present.");
        return qe::Undefined();
      }
      const std::string specifier = qe::ToWtf8(c, args[0]);
      if (qe::HasException(c)) return qe::Undefined();
      std::string resolved, error;
      if (!Resolve(realm, specifier, url, resolved, error)) {
        qe::ThrowTypeError(c, error);
        return qe::Undefined();
      }
      return qe::FromWtf8(c, resolved);
    });
    qe::Set(ctx, meta, "resolve", resolve);
  };
  hooks.dynamicImport = [](qe::Realm*, const std::string&, const std::string&, const std::string&) -> std::optional<std::string> { return std::nullopt; };
  return hooks;
}

}  // namespace solar::html
