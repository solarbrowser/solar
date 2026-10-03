#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace solar::net {

// What a Headers object is allowed to change (Fetch Standard, "guard").
enum class HeadersGuard { None, Request, RequestNoCors, Response, Immutable };

bool IsValidHeaderName(std::string_view name);
// After NormalizeHeaderValue: no NUL, CR or LF.
bool IsValidHeaderValue(std::string_view value);
// Without the HTTP whitespace (tab, LF, CR, space) at either end.
std::string NormalizeHeaderValue(std::string_view value);

// A script's string as the bytes of a header value, one byte for each code unit, which is how Web IDL's
// ByteString has it. None if a code point is above U+00FF, which the standard makes a TypeError.
std::optional<std::string> ToByteString(std::string_view utf8);
// The other way: each byte becomes the code point of the same number, as UTF-8.
std::string IsomorphicDecode(std::string_view bytes);

// Names a script's request cannot set because the browser is in charge of them, and what it cannot
// read of a response.
bool IsForbiddenRequestHeader(std::string_view name, std::string_view value);
bool IsForbiddenResponseHeaderName(std::string_view name);
// The few a no-cors request may carry, and only with values that cannot start a preflight.
bool IsNoCorsSafelistedRequestHeaderName(std::string_view name);
bool IsNoCorsSafelistedRequestHeader(std::string_view name, std::string_view value);

// A header list with the rules of the Headers class (Fetch Standard §2.2 and §5.2). Names and values
// are bytes, already a ByteString; the calls expect a valid name, and say whether a value is not.
class FetchHeaders {
 public:
  using Entry = std::pair<std::string, std::string>;
  enum class Status { Ok, InvalidValue, Immutable };

  explicit FetchHeaders(HeadersGuard guard = HeadersGuard::None) : guard_(guard) {}

  HeadersGuard guard() const { return guard_; }
  void SetGuard(HeadersGuard guard) { guard_ = guard; }

  // Add, replace and remove as the Headers methods do. A header the guard keeps out is left out
  // without a word, which is Ok.
  Status Append(std::string_view name, std::string_view value);
  Status Set(std::string_view name, std::string_view value);
  Status Delete(std::string_view name);

  // The value of the headers called `name`, joined with ", "; none if there are none.
  std::optional<std::string> Get(std::string_view name) const;
  bool Has(std::string_view name) const;
  std::vector<std::string> GetSetCookie() const;

  // What iteration shows: names in lower case, sorted, a name's values combined, except that every
  // Set-Cookie stands alone.
  std::vector<Entry> SortedAndCombined() const;

  // The list as it was filled, names as first written.
  const std::vector<Entry>& list() const { return list_; }

  // Adds without any of the guard's checks, for filling a Headers from a response or a request.
  void AppendUnchecked(std::string name, std::string value) { list_.emplace_back(std::move(name), std::move(value)); }

 private:
  // Whether the guard lets the header in.
  bool Allows(std::string_view name, std::string_view value) const;

  HeadersGuard guard_;
  std::vector<Entry> list_;
};

}  // namespace solar::net
