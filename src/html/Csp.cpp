#include "solar/html/Csp.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <vector>

#include "solar/html/Frames.h"
#include "solar/html/Modules.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::html::csp {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::vector<std::string> Words(std::string_view text) {
  std::vector<std::string> words;
  std::istringstream in{std::string(text)};
  std::string word;
  while (in >> word) words.push_back(word);
  return words;
}

// The source list that governs scripts in one policy: script-src, or else default-src. Absent: not limited.
std::optional<std::vector<std::string>> ScriptSources(const std::string& policy) {
  std::optional<std::vector<std::string>> scriptSrc, defaultSrc;
  std::istringstream in(policy);
  std::string directive;
  while (std::getline(in, directive, ';')) {
    std::vector<std::string> words = Words(directive);
    if (words.empty()) continue;
    const std::string name = Lower(words[0]);
    words.erase(words.begin());
    // The first of a name counts.
    if (name == "script-src" && !scriptSrc) scriptSrc = std::move(words);
    else if (name == "default-src" && !defaultSrc) defaultSrc = std::move(words);
  }
  return scriptSrc ? scriptSrc : defaultSrc;
}

bool Has(const std::vector<std::string>& sources, std::string_view keyword) {
  return std::any_of(sources.begin(), sources.end(), [&](const std::string& s) { return Lower(s) == keyword; });
}

bool HasNonceOrHash(const std::vector<std::string>& sources) {
  return std::any_of(sources.begin(), sources.end(), [](const std::string& s) {
    const std::string lower = Lower(s);
    return lower.starts_with("'nonce-") || lower.starts_with("'sha256-") || lower.starts_with("'sha384-") || lower.starts_with("'sha512-");
  });
}

bool HostMatches(const std::string& pattern, const std::string& host) {
  if (pattern == "*") return true;
  if (pattern.starts_with("*.")) return host.size() > pattern.size() - 1 && host.ends_with(pattern.substr(1));
  return pattern == host;
}

// Whether one source expression of a list lets the address of a script through.
bool SourceMatches(const std::string& source, const url::Url& address, const std::string& selfOrigin) {
  const std::string lower = Lower(source);
  if (lower == "'self'") return url::SerializeOrigin(address) == selfOrigin;
  if (lower == "*") return address.scheme == "http" || address.scheme == "https" || address.scheme == "ws" || address.scheme == "wss";
  if (lower.empty() || lower[0] == '\'') return false;
  if (lower.back() == ':') return address.scheme + ":" == lower || (lower == "https:" && address.scheme == "https");
  // [scheme://]host[:port][/path]
  std::string rest = lower;
  std::optional<std::string> scheme;
  if (const size_t at = rest.find("://"); at != std::string::npos) {
    scheme = rest.substr(0, at);
    rest = rest.substr(at + 3);
  }
  std::string path;
  if (const size_t slash = rest.find('/'); slash != std::string::npos) {
    path = rest.substr(slash);
    rest = rest.substr(0, slash);
  }
  std::string port;
  if (const size_t colon = rest.rfind(':'); colon != std::string::npos) {
    port = rest.substr(colon + 1);
    rest = rest.substr(0, colon);
  }
  if (scheme && *scheme != address.scheme && !(*scheme == "http" && address.scheme == "https")) return false;
  if (!scheme && address.scheme != "http" && address.scheme != "https") return false;
  if (!address.host || !HostMatches(rest, *address.host)) return false;
  if (!port.empty() && port != "*") {
    const std::optional<uint16_t> actual = address.port ? address.port : url::DefaultPort(address.scheme);
    if (!actual || std::to_string(*actual) != port) return false;
  } else if (port.empty() && address.port && address.port != url::DefaultPort(address.scheme)) {
    return false;
  }
  if (!path.empty()) {
    const std::string actual = "/" + [&] {
      std::string joined;
      for (const std::string& segment : address.path) joined += (joined.empty() ? "" : "/") + segment;
      return joined;
    }();
    if (path.back() == '/' ? !actual.starts_with(path) : actual != path) return false;
  }
  return true;
}

bool NonceMatches(const std::vector<std::string>& sources, const std::string& nonce) {
  if (nonce.empty()) return false;
  return Has(sources, "'nonce-" + nonce + "'") ||
         std::any_of(sources.begin(), sources.end(), [&](const std::string& s) { return s.size() > 8 && s.starts_with("'nonce-") && s.substr(7, s.size() - 8) == nonce && s.back() == '\''; });
}

}  // namespace

void AddPolicy(dom::Document* document, std::string_view policy) {
  if (document && !policy.empty()) document->policies.emplace_back(policy);
}

bool AllowsEval(const dom::Document* document) {
  if (!document) return true;
  for (const std::string& policy : document->policies) {
    const std::optional<std::vector<std::string>> sources = ScriptSources(policy);
    if (sources && !Has(*sources, "'unsafe-eval'")) return false;
  }
  return true;
}

bool AllowsInlineScript(const dom::Document* document, const std::string& nonce) {
  if (!document) return true;
  for (const std::string& policy : document->policies) {
    const std::optional<std::vector<std::string>> sources = ScriptSources(policy);
    if (!sources) continue;
    if (NonceMatches(*sources, nonce)) continue;
    // 'unsafe-inline' means nothing next to a nonce or a hash. Hashes are not computed here: such a script is refused.
    if (Has(*sources, "'unsafe-inline'") && !HasNonceOrHash(*sources)) continue;
    return false;
  }
  return true;
}

bool AllowsScriptUrl(const dom::Document* document, const std::string& address, const std::string& nonce) {
  if (!document) return true;
  const std::optional<url::Url> parsed = url::Parse(address);
  const std::string self = [&] {
    const std::optional<url::Url> own = url::Parse(DocumentBaseUrl(document));
    return own ? url::SerializeOrigin(*own) : std::string("null");
  }();
  for (const std::string& policy : document->policies) {
    const std::optional<std::vector<std::string>> sources = ScriptSources(policy);
    if (!sources) continue;
    if (NonceMatches(*sources, nonce)) continue;
    if (Has(*sources, "'none'") || !parsed) return false;
    const bool matched = std::any_of(sources->begin(), sources->end(), [&](const std::string& s) { return SourceMatches(s, *parsed, self); });
    if (!matched) return false;
  }
  return true;
}

Quanta::Embed::CodeGenerationHooks MakeCodeGenerationHooks() {
  Quanta::Embed::CodeGenerationHooks hooks;
  hooks.ensureCanCompile = [](Quanta::Embed::Realm* realm, Quanta::Embed::CompileKind, const std::vector<std::string>&) -> std::optional<std::string> {
    if (AllowsEval(DocumentOfRealm(realm))) return std::nullopt;
    return "Evaluating a string as JavaScript violates the following Content Security Policy directive because 'unsafe-eval' is not an allowed source of script";
  };
  return hooks;
}

}  // namespace solar::html::csp
