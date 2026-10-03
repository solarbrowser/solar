#pragma once

#include <string>
#include <vector>

namespace solar::test {

struct Identity {
  std::string certificatePem;
  std::string keyPem;
};

// A throwaway certificate authority, for tests that need a server the client can be told to trust
// (or not to), and certificates that are wrong in a chosen way.
class TestPki {
 public:
  TestPki();

  const std::string& rootPem() const { return rootPem_; }

  // A certificate for `names`, each a DNS name or an IP address literal, valid from
  // `notBeforeDays` to `notAfterDays` counted from now (negative is the past).
  Identity Issue(const std::vector<std::string>& names, int notBeforeDays = -1, int notAfterDays = 30) const;

 private:
  std::string rootPem_;
  std::string rootKeyPem_;
};

}  // namespace solar::test
