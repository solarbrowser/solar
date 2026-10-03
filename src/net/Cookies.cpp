#include "solar/net/Cookies.h"

#include <algorithm>

#include "solar/url/Host.h"
#include "solar/url/PublicSuffix.h"
#include "solar/url/Serializer.h"

namespace solar::net {

namespace {

using SystemClock = std::chrono::system_clock;
using namespace std::chrono_literals;

// RFC 6265bis limits: a cookie's name and value together, and each attribute's value.
constexpr size_t kMaxNameAndValue = 4096;
constexpr size_t kMaxAttributeValue = 1024;
// How far ahead a cookie may expire, however much its header asks.
constexpr auto kMaxLifetime = 400 * 24h;
// How many one registrable domain may keep, and how many the whole jar.
constexpr size_t kMaxPerDomain = 180;
constexpr size_t kMaxTotal = 3000;

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

std::string Lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = Lower(c);
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int64_t year, int month, int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

// The host `domain` names or sits under, which an IP address never does: only the same address.
bool DomainMatches(std::string_view host, std::string_view domain) {
  if (host == domain) return true;
  if (url::IsIpAddressHost(host) || host.size() <= domain.size()) return false;
  return host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.';
}

bool PathMatches(std::string_view requestPath, std::string_view cookiePath) {
  if (requestPath == cookiePath) return true;
  if (!requestPath.starts_with(cookiePath)) return false;
  return cookiePath.ends_with('/') || requestPath[cookiePath.size()] == '/';
}

// Where a cookie goes when its header names no path: the request's path up to its last slash.
std::string DefaultPath(std::string_view requestPath) {
  if (requestPath.empty() || requestPath.front() != '/') return "/";
  const size_t last = requestPath.rfind('/');
  return last == 0 ? "/" : std::string(requestPath.substr(0, last));
}

bool IsSecureUrl(const url::Url& url) { return url.scheme == "https" || url.scheme == "wss"; }

// Everything a Set-Cookie header says, before it is weighed against the request it answered.
struct ParsedCookie {
  Cookie cookie;
  std::optional<std::string> domain;
  std::optional<std::string> path;
  std::optional<SystemClock::time_point> expires;
  std::optional<int64_t> maxAge;
};

std::optional<ParsedCookie> ParseSetCookie(std::string_view text) {
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u <= 0x08) || (u >= 0x0A && u <= 0x1F) || u == 0x7F) return std::nullopt;
  }

  const size_t firstSemicolon = text.find(';');
  const std::string_view pair = text.substr(0, firstSemicolon);
  std::string_view attributes = firstSemicolon == std::string_view::npos ? std::string_view() : text.substr(firstSemicolon);

  ParsedCookie parsed;
  const size_t equals = pair.find('=');
  if (equals == std::string_view::npos) {
    parsed.cookie.value = std::string(Trim(pair));
  } else {
    parsed.cookie.name = std::string(Trim(pair.substr(0, equals)));
    parsed.cookie.value = std::string(Trim(pair.substr(equals + 1)));
  }
  if (parsed.cookie.name.empty() && parsed.cookie.value.empty()) return std::nullopt;
  if (parsed.cookie.name.size() + parsed.cookie.value.size() > kMaxNameAndValue) return std::nullopt;

  while (!attributes.empty()) {
    attributes.remove_prefix(1);  // the semicolon
    const size_t next = attributes.find(';');
    const std::string_view attribute = attributes.substr(0, next);
    attributes = next == std::string_view::npos ? std::string_view() : attributes.substr(next);

    const size_t eq = attribute.find('=');
    const std::string_view name = Trim(attribute.substr(0, eq));
    const std::string_view value = eq == std::string_view::npos ? std::string_view() : Trim(attribute.substr(eq + 1));
    if (value.size() > kMaxAttributeValue) continue;

    if (EqualsIgnoreCase(name, "expires")) {
      if (auto date = ParseCookieDate(value)) parsed.expires = *date;
    } else if (EqualsIgnoreCase(name, "max-age")) {
      if (value.empty() || !(IsAsciiDigit(value.front()) || value.front() == '-')) continue;
      const bool negative = value.front() == '-';
      const std::string_view digits = negative ? value.substr(1) : value;
      if (digits.empty() || !std::all_of(digits.begin(), digits.end(), IsAsciiDigit)) continue;
      int64_t seconds = 0;
      for (char c : digits) seconds = std::min<int64_t>(seconds * 10 + (c - '0'), int64_t{1} << 40);  // past any limit, short of overflowing here
      parsed.maxAge = negative ? -seconds : seconds;
    } else if (EqualsIgnoreCase(name, "domain")) {
      std::string_view domain = value;
      if (domain.starts_with('.')) domain.remove_prefix(1);
      if (!domain.empty()) parsed.domain = Lowered(domain);  // an empty one is ignored, and an earlier one stands
    } else if (EqualsIgnoreCase(name, "path")) {
      if (!value.empty() && value.front() == '/') {
        parsed.path = std::string(value);
      } else {
        parsed.path.reset();
      }
    } else if (EqualsIgnoreCase(name, "secure")) {
      parsed.cookie.secure = true;
    } else if (EqualsIgnoreCase(name, "httponly")) {
      parsed.cookie.httpOnly = true;
    } else if (EqualsIgnoreCase(name, "samesite")) {
      if (EqualsIgnoreCase(value, "strict")) parsed.cookie.sameSite = SameSite::Strict;
      else if (EqualsIgnoreCase(value, "lax")) parsed.cookie.sameSite = SameSite::Lax;
      else if (EqualsIgnoreCase(value, "none")) parsed.cookie.sameSite = SameSite::None;
      else parsed.cookie.sameSite = SameSite::Default;
    }
  }
  return parsed;
}

}  // namespace

std::optional<SystemClock::time_point> ParseCookieDate(std::string_view text) {
  std::optional<int> hour, minute, second, day, month, year;

  const auto isDelimiter = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u == 0x09 || (u >= 0x20 && u <= 0x2F) || (u >= 0x3B && u <= 0x40) || (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
  };
  // Up to `limit` digits from the start of `token`, and how many there were.
  const auto leadingNumber = [](std::string_view token, size_t limit, int& value) {
    size_t used = 0;
    value = 0;
    while (used < token.size() && used < limit && IsAsciiDigit(token[used])) value = value * 10 + (token[used++] - '0');
    return used;
  };

  static const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && isDelimiter(text[i])) ++i;
    const size_t start = i;
    while (i < text.size() && !isDelimiter(text[i])) ++i;
    const std::string_view token = text.substr(start, i - start);
    if (token.empty()) continue;

    if (!hour) {
      // hh:mm:ss, each of one or two digits, with anything after the last
      int h, m, s;
      size_t used = leadingNumber(token, 2, h);
      if (used > 0 && used < token.size() && token[used] == ':') {
        std::string_view rest = token.substr(used + 1);
        const size_t usedM = leadingNumber(rest, 2, m);
        if (usedM > 0 && usedM < rest.size() && rest[usedM] == ':') {
          rest.remove_prefix(usedM + 1);
          const size_t usedS = leadingNumber(rest, 2, s);
          if (usedS > 0 && (usedS == rest.size() || !IsAsciiDigit(rest[usedS]))) {
            hour = h;
            minute = m;
            second = s;
            continue;
          }
        }
      }
    }
    if (!day) {
      int d;
      const size_t used = leadingNumber(token, 2, d);
      if (used > 0 && (used == token.size() || !IsAsciiDigit(token[used]))) {
        day = d;
        continue;
      }
    }
    if (!month && token.size() >= 3) {
      const std::string lead = Lowered(token.substr(0, 3));
      bool found = false;
      for (int m = 0; m < 12 && !found; ++m) {
        if (lead == kMonths[m]) {
          month = m + 1;
          found = true;
        }
      }
      if (found) continue;
    }
    if (!year) {
      int y;
      const size_t used = leadingNumber(token, 4, y);
      if (used >= 2 && (used == token.size() || !IsAsciiDigit(token[used]))) {
        year = y;
        continue;
      }
    }
  }

  if (!hour || !day || !month || !year) return std::nullopt;
  if (*year >= 70 && *year <= 99) *year += 1900;
  else if (*year >= 0 && *year <= 69) *year += 2000;
  if (*day < 1 || *day > 31 || *year < 1601 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;

  const int64_t seconds = DaysFromCivil(*year, *month, *day) * 86400 + *hour * 3600 + *minute * 60 + *second;
  return SystemClock::time_point(std::chrono::seconds(seconds));
}

CookieJar::CookieJar(Clock clock) : clock_(clock ? std::move(clock) : Clock([] { return std::chrono::system_clock::now(); })) {}

void CookieJar::RemoveExpired(SystemClock::time_point now) {
  std::erase_if(cookies_, [now](const Cookie& c) { return c.expires && *c.expires <= now; });
}

void CookieJar::Store(const url::Url& requestUrl, std::string_view setCookie, const CookieRequest& request) {
  std::optional<ParsedCookie> parsedOr = ParseSetCookie(setCookie);
  if (!parsedOr || !requestUrl.host) return;
  ParsedCookie& parsed = *parsedOr;
  Cookie& cookie = parsed.cookie;
  const std::string& host = *requestUrl.host;
  const auto now = clock_();
  const bool secureRequest = IsSecureUrl(requestUrl);

  // Which host the cookie is for.
  if (parsed.domain) {
    std::string_view suffix = url::PublicSuffix(*parsed.domain);
    const bool isSuffix = !suffix.empty() && suffix.size() == parsed.domain->size();
    if (isSuffix) {
      // A cookie for "com" or "co.uk" would reach every site under it. Only the host that is that
      // name may have one, and then it is for that host alone.
      if (host != *parsed.domain) return;
      parsed.domain.reset();
    }
  }
  if (parsed.domain) {
    if (!DomainMatches(host, *parsed.domain)) return;
    cookie.domain = *parsed.domain;
    cookie.hostOnly = false;
  } else {
    cookie.domain = host;
    cookie.hostOnly = true;
  }
  const std::string requestPath = url::SerializePath(requestUrl);
  cookie.path = parsed.path ? *parsed.path : DefaultPath(requestPath);

  // How long it lasts. Max-Age beats Expires, and neither goes past the limit.
  if (parsed.maxAge) {
    // Cut to the limit before it is added to the time, which a header can make overflow.
    const auto lifetime = std::min<std::chrono::seconds>(std::chrono::seconds(*parsed.maxAge), kMaxLifetime);
    cookie.expires = *parsed.maxAge <= 0 ? SystemClock::time_point::min() : now + lifetime;
  } else if (parsed.expires) {
    cookie.expires = *parsed.expires;
  }
  if (cookie.expires && *cookie.expires > now + kMaxLifetime) cookie.expires = now + kMaxLifetime;

  // What the request may set.
  if (cookie.secure && !secureRequest) return;
  if (cookie.sameSite == SameSite::None && !cookie.secure) return;
  if ((cookie.sameSite == SameSite::Strict || cookie.sameSite == SameSite::Lax) && !request.sameSite) return;
  if (StartsWithIgnoreCase(cookie.name, "__Secure-") && !(cookie.secure && secureRequest)) return;
  if (StartsWithIgnoreCase(cookie.name, "__Host-") && !(cookie.secure && secureRequest && cookie.hostOnly && cookie.path == "/")) return;
  if (cookie.name.empty() && (StartsWithIgnoreCase(cookie.value, "__Secure-") || StartsWithIgnoreCase(cookie.value, "__Host-"))) return;

  RemoveExpired(now);
  // A page that is not secure must not overwrite a secure cookie, which could not have come from it.
  if (!secureRequest) {
    for (const Cookie& old : cookies_) {
      if (old.secure && old.name == cookie.name && (DomainMatches(cookie.domain, old.domain) || DomainMatches(old.domain, cookie.domain)) &&
          PathMatches(cookie.path, old.path)) {
        return;
      }
    }
  }

  cookie.created = now;
  cookie.lastAccessed = now;
  auto same = std::find_if(cookies_.begin(), cookies_.end(),
                           [&](const Cookie& c) { return c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path; });
  if (same != cookies_.end()) {
    cookie.created = same->created;  // it is the old cookie, changed
    if (cookie.expires && *cookie.expires <= now) {
      cookies_.erase(same);
      return;
    }
    *same = std::move(cookie);
    return;
  }
  if (cookie.expires && *cookie.expires <= now) return;
  cookies_.push_back(std::move(cookie));
  Evict(cookies_.back().domain);
}

// Over a limit the cookie that was used longest ago goes first.
void CookieJar::Evict(const std::string& domain) {
  const auto oldest = [this](const auto& belongs) {
    auto victim = cookies_.end();
    for (auto it = cookies_.begin(); it != cookies_.end(); ++it) {
      if (belongs(*it) && (victim == cookies_.end() || it->lastAccessed < victim->lastAccessed)) victim = it;
    }
    return victim;
  };
  const auto siteOf = [](const std::string& host) {
    const std::string_view site = url::RegistrableDomain(host);
    return site.empty() ? host : std::string(site);
  };

  const std::string site = siteOf(domain);
  while (static_cast<size_t>(std::count_if(cookies_.begin(), cookies_.end(), [&](const Cookie& c) { return siteOf(c.domain) == site; })) > kMaxPerDomain) {
    cookies_.erase(oldest([&](const Cookie& c) { return siteOf(c.domain) == site; }));
  }
  while (cookies_.size() > kMaxTotal) cookies_.erase(oldest([](const Cookie&) { return true; }));
}

std::vector<Cookie> CookieJar::Matching(const url::Url& requestUrl, const CookieRequest& request) {
  std::vector<Cookie> matches;
  if (!requestUrl.host) return matches;
  const std::string& host = *requestUrl.host;
  const std::string path = url::SerializePath(requestUrl);
  const bool secureRequest = IsSecureUrl(requestUrl);
  const auto now = clock_();
  RemoveExpired(now);

  for (Cookie& cookie : cookies_) {
    if (cookie.hostOnly ? host != cookie.domain : !DomainMatches(host, cookie.domain)) continue;
    if (!PathMatches(path, cookie.path)) continue;
    if (cookie.secure && !secureRequest) continue;
    // A cookie that does not say is held to Lax, so a cross-site request carries it only when it is
    // a navigation: the user went there, rather than a page sending the browser.
    if (!request.sameSite) {
      const bool lax = cookie.sameSite == SameSite::Lax || cookie.sameSite == SameSite::Default;
      if (cookie.sameSite == SameSite::Strict) continue;
      if (lax && !request.topLevelNavigation) continue;
    }
    cookie.lastAccessed = now;
    matches.push_back(cookie);
  }
  // Longer paths first, then the older cookie; the sort is stable, so the jar's own order breaks ties.
  std::stable_sort(matches.begin(), matches.end(), [](const Cookie& a, const Cookie& b) {
    if (a.path.size() != b.path.size()) return a.path.size() > b.path.size();
    return a.created < b.created;
  });
  return matches;
}

std::string CookieJar::HeaderFor(const url::Url& requestUrl, const CookieRequest& request) {
  std::string header;
  for (const Cookie& cookie : Matching(requestUrl, request)) {
    if (!header.empty()) header += "; ";
    header += cookie.name.empty() ? cookie.value : cookie.name + "=" + cookie.value;
  }
  return header;
}

}  // namespace solar::net
