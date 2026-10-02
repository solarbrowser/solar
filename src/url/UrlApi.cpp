#include "solar/url/UrlApi.h"

#include "solar/url/Parser.h"
#include "solar/url/PercentEncode.h"
#include "solar/url/Serializer.h"

namespace solar::url {

std::string GetProtocol(const Url& url) { return url.scheme + ":"; }

std::string GetUsername(const Url& url) { return url.username; }

std::string GetPassword(const Url& url) { return url.password; }

std::string GetHost(const Url& url) {
  if (!url.host) return "";
  if (!url.port) return *url.host;
  return *url.host + ":" + std::to_string(*url.port);
}

std::string GetHostname(const Url& url) { return url.host.value_or(""); }

std::string GetPort(const Url& url) { return url.port ? std::to_string(*url.port) : ""; }

std::string GetPathname(const Url& url) { return SerializePath(url); }

std::string GetSearch(const Url& url) {
  if (!url.query || url.query->empty()) return "";
  return "?" + *url.query;
}

std::string GetHash(const Url& url) {
  if (!url.fragment || url.fragment->empty()) return "";
  return "#" + *url.fragment;
}

void SetProtocol(Url& url, std::string_view value) {
  ParseInto(url, std::string(value) + ":", StateOverride::SchemeStart);
}

void SetUsername(Url& url, std::string_view value) {
  if (url.CannotHaveUsernamePasswordPort()) return;
  url.username.clear();
  AppendPercentEncoded(url.username, value, EncodeSet::Userinfo);
}

void SetPassword(Url& url, std::string_view value) {
  if (url.CannotHaveUsernamePasswordPort()) return;
  url.password.clear();
  AppendPercentEncoded(url.password, value, EncodeSet::Userinfo);
}

void SetHost(Url& url, std::string_view value) {
  if (url.opaquePath) return;
  ParseInto(url, value, StateOverride::Host);
}

void SetHostname(Url& url, std::string_view value) {
  if (url.opaquePath) return;
  ParseInto(url, value, StateOverride::Hostname);
}

void SetPort(Url& url, std::string_view value) {
  if (url.CannotHaveUsernamePasswordPort()) return;
  if (value.empty()) {
    url.port.reset();
    return;
  }
  ParseInto(url, value, StateOverride::Port);
}

void SetPathname(Url& url, std::string_view value) {
  if (url.opaquePath) return;
  url.path.clear();
  ParseInto(url, value, StateOverride::PathStart);
}

void SetSearch(Url& url, std::string_view value) {
  if (value.empty()) {
    url.query.reset();
    return;
  }
  if (value.front() == '?') value.remove_prefix(1);
  url.query = "";
  ParseInto(url, value, StateOverride::Query);
}

void SetHash(Url& url, std::string_view value) {
  if (value.empty()) {
    url.fragment.reset();
    return;
  }
  if (value.front() == '#') value.remove_prefix(1);
  url.fragment = "";
  ParseInto(url, value, StateOverride::Fragment);
}

}  // namespace solar::url
