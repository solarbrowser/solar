#include "solar/net/Cors.h"

#include <algorithm>
#include <functional>

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

std::string_view Trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

size_t CountHeaders(const HttpResponseHead& head, std::string_view name) {
  return static_cast<size_t>(std::count_if(head.headers.begin(), head.headers.end(), [&](const auto& h) { return EqualsIgnoreCase(h.first, name); }));
}

// The comma-separated values of every header called `name`.
std::vector<std::string> ListOf(const HttpResponseHead& head, std::string_view name) {
  std::vector<std::string> out;
  for (const auto& [key, value] : head.headers) {
    if (!EqualsIgnoreCase(key, name)) continue;
    size_t start = 0;
    while (start <= value.size()) {
      size_t end = value.find(',', start);
      if (end == std::string::npos) end = value.size();
      const std::string_view token = Trimmed(std::string_view(value).substr(start, end - start));
      if (!token.empty()) out.emplace_back(token);
      start = end + 1;
    }
  }
  return out;
}

}  // namespace

bool CorsAllowsResponse(const HttpResponseHead& response, std::string_view origin, bool includeCredentials) {
  if (CountHeaders(response, "access-control-allow-origin") != 1) return false;
  const std::string_view allowed = *response.Header("access-control-allow-origin");
  if (!includeCredentials && allowed == "*") return true;
  if (allowed != origin) return false;
  if (!includeCredentials) return true;
  const auto credentials = response.Header("access-control-allow-credentials");
  return credentials && *credentials == "true";
}

bool IsCorsExposedHeader(std::string_view name, const HttpResponseHead& response, bool includeCredentials) {
  for (const char* safe : {"cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma"}) {
    if (EqualsIgnoreCase(name, safe)) return true;
  }
  for (const std::string& token : ListOf(response, "access-control-expose-headers")) {
    if (token == "*" && !includeCredentials) return true;
    if (EqualsIgnoreCase(token, name)) return true;
  }
  return false;
}

std::vector<std::string> CorsUnsafeRequestHeaderNames(const FetchHeaders& headers) {
  std::vector<std::string> names;
  for (const auto& [name, value] : headers.list()) {
    if (!IsNoCorsSafelistedRequestHeader(name, value)) names.push_back(Lowered(name));
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

bool NeedsPreflight(std::string_view method, const FetchHeaders& headers) {
  if (method != "GET" && method != "HEAD" && method != "POST") return true;
  return !CorsUnsafeRequestHeaderNames(headers).empty();
}

bool PreflightPermission::AllowsMethod(std::string_view method) const {
  return method == "GET" || method == "HEAD" || method == "POST" || anyMethod || methods.count(std::string(method)) > 0;
}

bool PreflightPermission::AllowsHeader(std::string_view name) const {
  // "*" never covers Authorization, which has to be named.
  return headers.count(std::string(name)) > 0 || (anyHeader && name != "authorization");
}

PreflightVerdict CheckPreflightResponse(const HttpResponseHead& response, std::string_view origin, bool includeCredentials, std::string_view method,
                                        const std::vector<std::string>& unsafeHeaders) {
  PreflightVerdict verdict;
  if (!CorsAllowsResponse(response, origin, includeCredentials)) {
    verdict.reason = "the preflight response does not allow this origin";
    return verdict;
  }
  if (response.status < 200 || response.status > 299) {
    verdict.reason = "the preflight response has status " + std::to_string(response.status);
    return verdict;
  }

  PreflightPermission& permission = verdict.permission;
  for (const std::string& token : ListOf(response, "access-control-allow-methods")) {
    if (token == "*" && !includeCredentials) {
      permission.anyMethod = true;
    } else {
      permission.methods.insert(token);
    }
  }
  for (const std::string& token : ListOf(response, "access-control-allow-headers")) {
    if (token == "*" && !includeCredentials) {
      permission.anyHeader = true;
    } else {
      permission.headers.insert(Lowered(token));
    }
  }
  if (const auto maxAge = response.Header("access-control-max-age")) {
    int64_t seconds = 0;
    bool valid = !maxAge->empty();
    for (char c : *maxAge) {
      if (c < '0' || c > '9') valid = false;
      else seconds = std::min<int64_t>(seconds * 10 + (c - '0'), 1 << 30);
    }
    if (valid) permission.maxAge = std::chrono::seconds(std::min<int64_t>(seconds, 7200));
  }

  if (!permission.AllowsMethod(method)) {
    verdict.reason = "the method " + std::string(method) + " is not allowed by the preflight";
    return verdict;
  }
  for (const std::string& name : unsafeHeaders) {
    if (!permission.AllowsHeader(name)) {
      verdict.reason = "the header " + name + " is not allowed by the preflight";
      return verdict;
    }
  }
  verdict.ok = true;
  return verdict;
}

PreflightCache::PreflightCache(Clock clock) : clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })) {}

void PreflightCache::Store(const std::string& origin, const std::string& url, bool includeCredentials, PreflightPermission permission) {
  if (permission.maxAge.count() <= 0) return;
  if (entries_.size() > 256) entries_.clear();  // a bound; asking again costs one request
  entries_[origin + " " + url + (includeCredentials ? " include" : " omit")] = {permission, clock_() + permission.maxAge};
}

bool PreflightCache::Allows(const std::string& origin, const std::string& url, bool includeCredentials, std::string_view method,
                            const std::vector<std::string>& unsafeHeaders) {
  auto it = entries_.find(origin + " " + url + (includeCredentials ? " include" : " omit"));
  if (it == entries_.end()) return false;
  if (it->second.expires <= clock_()) {
    entries_.erase(it);
    return false;
  }
  if (!it->second.permission.AllowsMethod(method)) return false;
  return std::all_of(unsafeHeaders.begin(), unsafeHeaders.end(), [&](const std::string& name) { return it->second.permission.AllowsHeader(name); });
}

}  // namespace solar::net
