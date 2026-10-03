#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace solar::net {

// The hosts that have said, with a Strict-Transport-Security header (RFC 6797), that they are to be
// reached over https only. Held in memory for as long as the store lives.
class HstsStore {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  // `clock` is for tests; the system's by default.
  explicit HstsStore(Clock clock = nullptr);

  // A response from `host` came over a TLS connection whose certificate was verified, and carried
  // this Strict-Transport-Security value. A value that is not valid is ignored whole, as the RFC
  // says, and so is anything from an IP address, which a certificate does not make anyone's.
  void Note(std::string_view host, std::string_view headerValue);

  // Whether `host`, a host as a URL serializes it, is to be reached over https: it has an entry, or
  // a domain above it has one that covers its subdomains.
  bool Covers(std::string_view host) const;

 private:
  struct Entry {
    std::chrono::system_clock::time_point expires;
    bool includeSubdomains;
  };

  Clock clock_;
  std::map<std::string, Entry, std::less<>> entries_;
};

}  // namespace solar::net
