#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "solar/net/FetchHeaders.h"
#include "solar/net/Http1Parser.h"

namespace solar::net {

// The rules of CORS (Fetch Standard §3.2 and §4.8) that do not depend on a script engine or a network.

// The CORS check of a response: it names this origin, or anyone if the request carried no credentials,
// and says so about credentials when it did. `origin` is "null" for an opaque one.
bool CorsAllowsResponse(const HttpResponseHead& response, std::string_view origin, bool includeCredentials);

// Whether a script may see the header of a cors response: the CORS-safelisted ones, and those the server
// named in Access-Control-Expose-Headers (or all, with "*", if the request had no credentials).
bool IsCorsExposedHeader(std::string_view name, const HttpResponseHead& response, bool includeCredentials);

// The names, lower case, sorted and without repeats, of the headers a cors request may not send unless a
// preflight has allowed them.
std::vector<std::string> CorsUnsafeRequestHeaderNames(const FetchHeaders& headers);

// Whether a cors request has to ask first: its method is not one of the three simple ones, or it carries
// an unsafe header.
bool NeedsPreflight(std::string_view method, const FetchHeaders& headers);

// What a server allowed in answer to a preflight.
struct PreflightPermission {
  std::set<std::string> methods;  // as the server wrote them; methods are case sensitive
  std::set<std::string> headers;  // lower case
  bool anyMethod = false;         // "*", which counts only without credentials
  bool anyHeader = false;
  std::chrono::seconds maxAge{5};

  bool AllowsMethod(std::string_view method) const;
  bool AllowsHeader(std::string_view lowerCaseName) const;
};

struct PreflightVerdict {
  bool ok = false;
  std::string reason;  // when it is not
  PreflightPermission permission;
};

// Weighs a preflight's response (RFC: Fetch §4.8 step 7): a CORS check, a successful status, and the
// methods and headers the request wants. `unsafeHeaders` is CorsUnsafeRequestHeaderNames of its headers.
PreflightVerdict CheckPreflightResponse(const HttpResponseHead& response, std::string_view origin, bool includeCredentials, std::string_view method,
                                        const std::vector<std::string>& unsafeHeaders);

// What preflights have allowed, so that the next request like them does not ask again (until the server's
// Access-Control-Max-Age is up, 5 seconds if it gave none; at most 2 hours).
class PreflightCache {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  explicit PreflightCache(Clock clock = nullptr);

  void Store(const std::string& origin, const std::string& url, bool includeCredentials, PreflightPermission permission);
  // Whether a stored permission covers this request.
  bool Allows(const std::string& origin, const std::string& url, bool includeCredentials, std::string_view method, const std::vector<std::string>& unsafeHeaders);

 private:
  struct Entry {
    PreflightPermission permission;
    std::chrono::steady_clock::time_point expires;
  };
  Clock clock_;
  std::map<std::string, Entry> entries_;
};

}  // namespace solar::net
