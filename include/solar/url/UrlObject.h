#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "solar/url/Url.h"
#include "solar/url/UrlSearchParams.h"

namespace solar::url {

// The standard's URL interface: a URL record plus the query object that mirrors its query.
// Fixed in place because the query object points back at the record.
class UrlObject {
 public:
  UrlObject(const UrlObject&) = delete;
  UrlObject& operator=(const UrlObject&) = delete;

  // Null where the standard's constructor would throw a TypeError.
  static std::unique_ptr<UrlObject> Create(std::string_view url, std::optional<std::string_view> base = std::nullopt);

  const Url& Record() const { return url_; }
  UrlSearchParams& SearchParams() { return params_; }

  std::string Href() const;
  std::string Origin() const;
  std::string Protocol() const;
  std::string Username() const;
  std::string Password() const;
  std::string Host() const;
  std::string Hostname() const;
  std::string Port() const;
  std::string Pathname() const;
  std::string Search() const;
  std::string Hash() const;

  // False where the standard's href setter would throw.
  bool SetHref(std::string_view value);
  void SetProtocol(std::string_view value);
  void SetUsername(std::string_view value);
  void SetPassword(std::string_view value);
  void SetHost(std::string_view value);
  void SetHostname(std::string_view value);
  void SetPort(std::string_view value);
  void SetPathname(std::string_view value);
  void SetSearch(std::string_view value);
  void SetHash(std::string_view value);

 private:
  explicit UrlObject(Url url);
  void SyncQueryObject();

  Url url_;
  UrlSearchParams params_;
};

}  // namespace solar::url
