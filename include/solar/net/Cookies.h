#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "solar/url/Url.h"

namespace solar::net {

enum class SameSite { Default, None, Lax, Strict };

struct Cookie {
  std::string name;  // empty for a cookie that came as a bare value
  std::string value;
  std::string domain;  // lower case, without a leading dot
  std::string path;
  std::optional<std::chrono::system_clock::time_point> expires;  // none for a session cookie
  std::chrono::system_clock::time_point created;
  std::chrono::system_clock::time_point lastAccessed;
  bool hostOnly = false;  // sent only to the host that set it, not to its subdomains
  bool secure = false;
  bool httpOnly = false;
  SameSite sameSite = SameSite::Default;
};

// What a request is, as far as which cookies it may carry.
struct CookieRequest {
  // Its URL and everything it was redirected through are same-site with the page that made it.
  bool sameSite = true;
  // It loads a page in the top-level browsing context, so a Lax cookie may go along cross-site.
  bool topLevelNavigation = false;
};

// The cookies a browser keeps (RFC 6265bis): what Set-Cookie headers give it and what it puts in
// Cookie headers. In memory, for as long as the jar lives.
class CookieJar {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  // `clock` is for tests; the system's by default.
  explicit CookieJar(Clock clock = nullptr);

  // `setCookie` came in a response to a request for `url`. A cookie that the rules refuse, a
  // malformed one, one that has already expired and the like change nothing but what they delete.
  void Store(const url::Url& url, std::string_view setCookie, const CookieRequest& request);

  // The cookies a request for `url` carries, in the order they go in the header.
  std::vector<Cookie> Matching(const url::Url& url, const CookieRequest& request);
  // The Cookie header's value for those, empty if there are none.
  std::string HeaderFor(const url::Url& url, const CookieRequest& request);

  size_t size() const { return cookies_.size(); }

 private:
  void RemoveExpired(std::chrono::system_clock::time_point now);
  void Evict(const std::string& domain);

  Clock clock_;
  std::vector<Cookie> cookies_;  // oldest first
};

}  // namespace solar::net
