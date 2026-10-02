#include "solar/url/UrlObject.h"

#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"

namespace solar::url {

UrlObject::UrlObject(Url url) : url_(std::move(url)) {
  params_.url_ = &url_;
  SyncQueryObject();
}

std::unique_ptr<UrlObject> UrlObject::Create(std::string_view url, std::optional<std::string_view> base) {
  std::optional<Url> parsedBase;
  if (base) {
    parsedBase = Parse(*base);
    if (!parsedBase) return nullptr;
  }
  std::optional<Url> parsed = Parse(url, parsedBase ? &*parsedBase : nullptr);
  if (!parsed) return nullptr;
  return std::unique_ptr<UrlObject>(new UrlObject(std::move(*parsed)));
}

std::string UrlObject::Href() const { return Serialize(url_); }
std::string UrlObject::Origin() const { return SerializeOrigin(url_); }
std::string UrlObject::Protocol() const { return GetProtocol(url_); }
std::string UrlObject::Username() const { return GetUsername(url_); }
std::string UrlObject::Password() const { return GetPassword(url_); }
std::string UrlObject::Host() const { return GetHost(url_); }
std::string UrlObject::Hostname() const { return GetHostname(url_); }
std::string UrlObject::Port() const { return GetPort(url_); }
std::string UrlObject::Pathname() const { return GetPathname(url_); }
std::string UrlObject::Search() const { return GetSearch(url_); }
std::string UrlObject::Hash() const { return GetHash(url_); }

bool UrlObject::SetHref(std::string_view value) {
  std::optional<Url> parsed = Parse(value);
  if (!parsed) return false;
  url_ = std::move(*parsed);
  SyncQueryObject();
  return true;
}

void UrlObject::SetProtocol(std::string_view value) { solar::url::SetProtocol(url_, value); }
void UrlObject::SetUsername(std::string_view value) { solar::url::SetUsername(url_, value); }
void UrlObject::SetPassword(std::string_view value) { solar::url::SetPassword(url_, value); }
void UrlObject::SetHost(std::string_view value) { solar::url::SetHost(url_, value); }
void UrlObject::SetHostname(std::string_view value) { solar::url::SetHostname(url_, value); }
void UrlObject::SetPort(std::string_view value) { solar::url::SetPort(url_, value); }
void UrlObject::SetPathname(std::string_view value) { solar::url::SetPathname(url_, value); }
void UrlObject::SetHash(std::string_view value) { solar::url::SetHash(url_, value); }

void UrlObject::SetSearch(std::string_view value) {
  solar::url::SetSearch(url_, value);
  if (value.starts_with('?')) value.remove_prefix(1);
  // The list comes from the raw input, not the percent-encoded query that was stored.
  params_.ReplaceFromQuery(value);
}

void UrlObject::SyncQueryObject() { params_.ReplaceFromQuery(url_.query.value_or("")); }

}  // namespace solar::url
