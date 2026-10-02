#pragma once

#include <string>
#include <string_view>

#include "solar/url/Url.h"

// The attributes of the standard's URL interface, as functions on a URL record.
// The query object (searchParams) is not kept in sync yet.
namespace solar::url {

std::string GetProtocol(const Url& url);
std::string GetUsername(const Url& url);
std::string GetPassword(const Url& url);
std::string GetHost(const Url& url);
std::string GetHostname(const Url& url);
std::string GetPort(const Url& url);
std::string GetPathname(const Url& url);
std::string GetSearch(const Url& url);
std::string GetHash(const Url& url);

void SetProtocol(Url& url, std::string_view value);
void SetUsername(Url& url, std::string_view value);
void SetPassword(Url& url, std::string_view value);
void SetHost(Url& url, std::string_view value);
void SetHostname(Url& url, std::string_view value);
void SetPort(Url& url, std::string_view value);
void SetPathname(Url& url, std::string_view value);
void SetSearch(Url& url, std::string_view value);
void SetHash(Url& url, std::string_view value);

}  // namespace solar::url
