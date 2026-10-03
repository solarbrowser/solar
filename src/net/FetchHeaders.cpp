#include "solar/net/FetchHeaders.h"

#include <algorithm>

namespace solar::net {

namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

std::string Lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = Lower(c);
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

bool IsTokenChar(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool IsHttpWhitespace(char c) { return c == '\t' || c == '\n' || c == '\r' || c == ' '; }

std::string_view Trimmed(std::string_view text) {
  while (!text.empty() && IsHttpWhitespace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsHttpWhitespace(text.back())) text.remove_suffix(1);
  return text;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

// A byte that makes a value one a cross-origin request may not carry unchecked.
bool IsCorsUnsafeByte(unsigned char c) {
  if (c <= 0x08 || (c >= 0x0A && c <= 0x1F) || c == 0x7F) return true;
  return std::string_view("\"():<>?@[\\]{}").find(static_cast<char>(c)) != std::string_view::npos;
}

}  // namespace

bool IsValidHeaderName(std::string_view name) { return !name.empty() && std::all_of(name.begin(), name.end(), IsTokenChar); }

bool IsValidHeaderValue(std::string_view value) {
  return std::none_of(value.begin(), value.end(), [](char c) { return c == '\0' || c == '\n' || c == '\r'; });
}

std::string NormalizeHeaderValue(std::string_view value) { return std::string(Trimmed(value)); }

std::optional<std::string> ToByteString(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if ((c == 0xC2 || c == 0xC3) && i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80) {
      // U+0080 to U+00FF are the two-byte sequences that start with C2 or C3.
      out.push_back(static_cast<char>(((c & 0x03) << 6) | (static_cast<unsigned char>(utf8[++i]) & 0x3F)));
    } else {
      return std::nullopt;
    }
  }
  return out;
}

std::string IsomorphicDecode(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (char b : bytes) {
    const unsigned char c = static_cast<unsigned char>(b);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

bool IsForbiddenRequestHeader(std::string_view name, std::string_view value) {
  static const char* const kForbidden[] = {"accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
                                           "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
                                           "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via"};
  for (const char* forbidden : kForbidden) {
    if (EqualsIgnoreCase(name, forbidden)) return true;
  }
  if (StartsWithIgnoreCase(name, "proxy-") || StartsWithIgnoreCase(name, "sec-")) return true;

  // A header that asks a proxy to change the method must not ask for one that is forbidden.
  if (EqualsIgnoreCase(name, "x-http-method") || EqualsIgnoreCase(name, "x-http-method-override") || EqualsIgnoreCase(name, "x-method-override")) {
    size_t start = 0;
    while (start <= value.size()) {
      size_t end = value.find(',', start);
      if (end == std::string_view::npos) end = value.size();
      const std::string_view method = Trimmed(value.substr(start, end - start));
      if (EqualsIgnoreCase(method, "CONNECT") || EqualsIgnoreCase(method, "TRACE") || EqualsIgnoreCase(method, "TRACK")) return true;
      start = end + 1;
    }
  }
  return false;
}

bool IsForbiddenResponseHeaderName(std::string_view name) { return EqualsIgnoreCase(name, "set-cookie") || EqualsIgnoreCase(name, "set-cookie2"); }

bool IsNoCorsSafelistedRequestHeader(std::string_view name, std::string_view value) {
  if (value.size() > 128) return false;
  if (EqualsIgnoreCase(name, "accept")) return std::none_of(value.begin(), value.end(), [](char c) { return IsCorsUnsafeByte(static_cast<unsigned char>(c)); });
  if (EqualsIgnoreCase(name, "accept-language") || EqualsIgnoreCase(name, "content-language")) {
    return std::all_of(value.begin(), value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '*' || c == ',' || c == '-' || c == '.' ||
             c == ';' || c == '=';
    });
  }
  if (EqualsIgnoreCase(name, "content-type")) {
    if (std::any_of(value.begin(), value.end(), [](char c) { return IsCorsUnsafeByte(static_cast<unsigned char>(c)); })) return false;
    const std::string_view essence = Trimmed(value.substr(0, value.find(';')));
    const std::string lowered = Lowered(essence);
    return lowered == "application/x-www-form-urlencoded" || lowered == "multipart/form-data" || lowered == "text/plain";
  }
  return false;
}

bool FetchHeaders::Allows(std::string_view name, std::string_view value) const {
  if (guard_ == HeadersGuard::Request && IsForbiddenRequestHeader(name, value)) return false;
  if (guard_ == HeadersGuard::Response && IsForbiddenResponseHeaderName(name)) return false;
  return true;
}

FetchHeaders::Status FetchHeaders::Append(std::string_view name, std::string_view rawValue) {
  const std::string value = NormalizeHeaderValue(rawValue);
  if (!IsValidHeaderValue(value)) return Status::InvalidValue;
  if (guard_ == HeadersGuard::Immutable) return Status::Immutable;
  if (!Allows(name, value)) return Status::Ok;
  if (guard_ == HeadersGuard::RequestNoCors) {
    // What the request would then carry has to be one a no-cors request may.
    const std::optional<std::string> existing = Get(name);
    const std::string combined = existing ? *existing + ", " + value : value;
    if (!IsNoCorsSafelistedRequestHeader(name, combined)) return Status::Ok;
  }
  list_.emplace_back(std::string(name), value);
  return Status::Ok;
}

FetchHeaders::Status FetchHeaders::Set(std::string_view name, std::string_view rawValue) {
  const std::string value = NormalizeHeaderValue(rawValue);
  if (!IsValidHeaderValue(value)) return Status::InvalidValue;
  if (guard_ == HeadersGuard::Immutable) return Status::Immutable;
  if (!Allows(name, value)) return Status::Ok;
  if (guard_ == HeadersGuard::RequestNoCors && !IsNoCorsSafelistedRequestHeader(name, value)) return Status::Ok;

  // The first header of that name takes the value and stays where it is; the others go.
  bool replaced = false;
  for (auto it = list_.begin(); it != list_.end();) {
    if (!EqualsIgnoreCase(it->first, name)) {
      ++it;
    } else if (!replaced) {
      it->second = value;
      replaced = true;
      ++it;
    } else {
      it = list_.erase(it);
    }
  }
  if (!replaced) list_.emplace_back(std::string(name), value);
  return Status::Ok;
}

FetchHeaders::Status FetchHeaders::Delete(std::string_view name) {
  if (guard_ == HeadersGuard::Immutable) return Status::Immutable;
  if (!Allows(name, "")) return Status::Ok;
  if (guard_ == HeadersGuard::RequestNoCors && !IsNoCorsSafelistedRequestHeader(name, "")) return Status::Ok;
  std::erase_if(list_, [&](const Entry& e) { return EqualsIgnoreCase(e.first, name); });
  return Status::Ok;
}

std::optional<std::string> FetchHeaders::Get(std::string_view name) const {
  std::optional<std::string> out;
  for (const Entry& entry : list_) {
    if (!EqualsIgnoreCase(entry.first, name)) continue;
    out = out ? *out + ", " + entry.second : entry.second;
  }
  return out;
}

bool FetchHeaders::Has(std::string_view name) const {
  return std::any_of(list_.begin(), list_.end(), [&](const Entry& e) { return EqualsIgnoreCase(e.first, name); });
}

std::vector<std::string> FetchHeaders::GetSetCookie() const {
  std::vector<std::string> out;
  for (const Entry& entry : list_) {
    if (EqualsIgnoreCase(entry.first, "set-cookie")) out.push_back(entry.second);
  }
  return out;
}

std::vector<FetchHeaders::Entry> FetchHeaders::SortedAndCombined() const {
  std::vector<std::string> names;
  for (const Entry& entry : list_) names.push_back(Lowered(entry.first));
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::vector<Entry> out;
  for (const std::string& name : names) {
    if (name == "set-cookie") {
      for (const std::string& value : GetSetCookie()) out.emplace_back(name, value);
    } else {
      out.emplace_back(name, *Get(name));
    }
  }
  return out;
}

}  // namespace solar::net
