#include "solar/url/Parser.h"

#include <cstddef>

#include "solar/url/Host.h"
#include "solar/url/PercentEncode.h"
#include "solar/url/Utf8.h"

namespace solar::url {

namespace {

constexpr int kEof = -1;

enum class State {
  SchemeStart,
  Scheme,
  NoScheme,
  SpecialRelativeOrAuthority,
  PathOrAuthority,
  Relative,
  RelativeSlash,
  SpecialAuthoritySlashes,
  SpecialAuthorityIgnoreSlashes,
  Authority,
  Host,
  Hostname,  // only ever a state override: identical to Host except that it refuses a port
  Port,
  File,
  FileSlash,
  FileHost,
  PathStart,
  Path,
  OpaquePath,
  Query,
  Fragment,
};

bool IsAsciiAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsAsciiDigit(int c) { return c >= '0' && c <= '9'; }
bool IsAsciiAlphanumeric(int c) { return IsAsciiAlpha(c) || IsAsciiDigit(c); }

char AsciiLower(int c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 0x20 : c); }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiLower(static_cast<unsigned char>(a[i])) != AsciiLower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool IsSingleDotSegment(std::string_view s) { return s == "." || EqualsIgnoreCase(s, "%2e"); }

bool IsDoubleDotSegment(std::string_view s) {
  return s == ".." || EqualsIgnoreCase(s, ".%2e") || EqualsIgnoreCase(s, "%2e.") ||
         EqualsIgnoreCase(s, "%2e%2e");
}

class BasicUrlParser {
 public:
  BasicUrlParser(std::string_view input, const Url* base, Url& url, std::optional<State> stateOverride)
      : input_(input),
        base_(base),
        url_(url),
        override_(stateOverride),
        state_(stateOverride.value_or(State::SchemeStart)) {}

  bool Run() {
    const std::ptrdiff_t size = static_cast<std::ptrdiff_t>(input_.size());
    while (true) {
      if (!Step()) return false;
      if (done_ || pointer_ >= size) break;
      ++pointer_;
    }
    return true;
  }

 private:
  int C() const {
    if (pointer_ < 0 || pointer_ >= static_cast<std::ptrdiff_t>(input_.size())) return kEof;
    return static_cast<unsigned char>(input_[pointer_]);
  }

  bool RemainingStartsWith(std::string_view prefix) const {
    size_t start = static_cast<size_t>(pointer_ + 1);
    return start <= input_.size() && input_.substr(start).starts_with(prefix);
  }

  bool IsSpecialTerminator(int c) const {
    return c == kEof || c == '/' || c == '?' || c == '#' || (url_.IsSpecial() && c == '\\');
  }

  bool Step() {
    switch (state_) {
      case State::SchemeStart: return OnSchemeStart();
      case State::Scheme: return OnScheme();
      case State::NoScheme: return OnNoScheme();
      case State::SpecialRelativeOrAuthority: return OnSpecialRelativeOrAuthority();
      case State::PathOrAuthority: return OnPathOrAuthority();
      case State::Relative: return OnRelative();
      case State::RelativeSlash: return OnRelativeSlash();
      case State::SpecialAuthoritySlashes: return OnSpecialAuthoritySlashes();
      case State::SpecialAuthorityIgnoreSlashes: return OnSpecialAuthorityIgnoreSlashes();
      case State::Authority: return OnAuthority();
      case State::Host:
      case State::Hostname: return OnHost();
      case State::Port: return OnPort();
      case State::File: return OnFile();
      case State::FileSlash: return OnFileSlash();
      case State::FileHost: return OnFileHost();
      case State::PathStart: return OnPathStart();
      case State::Path: return OnPath();
      case State::OpaquePath: return OnOpaquePath();
      case State::Query: return OnQuery();
      case State::Fragment: return OnFragment();
    }
    return false;
  }

  bool OnSchemeStart() {
    int c = C();
    if (IsAsciiAlpha(c)) {
      buffer_.push_back(AsciiLower(c));
      state_ = State::Scheme;
    } else if (!override_) {
      state_ = State::NoScheme;
      --pointer_;
    } else {
      return false;
    }
    return true;
  }

  bool OnScheme() {
    int c = C();
    if (IsAsciiAlphanumeric(c) || c == '+' || c == '-' || c == '.') {
      buffer_.push_back(AsciiLower(c));
    } else if (c == ':') {
      if (override_) {
        // A scheme change must not cross between special and non-special, and file
        // keeps its restrictions on credentials, ports and an empty host.
        if (url_.IsSpecial() != IsSpecialScheme(buffer_)) return Finish();
        if ((url_.IncludesCredentials() || url_.port) && buffer_ == "file") return Finish();
        if (url_.scheme == "file" && url_.host && url_.host->empty()) return Finish();
      }
      url_.scheme = std::move(buffer_);
      buffer_.clear();

      if (override_) {
        if (url_.port == DefaultPort(url_.scheme)) url_.port.reset();
        return Finish();
      }

      if (url_.scheme == "file") {
        state_ = State::File;
      } else if (url_.IsSpecial() && base_ && base_->scheme == url_.scheme) {
        state_ = State::SpecialRelativeOrAuthority;
      } else if (url_.IsSpecial()) {
        state_ = State::SpecialAuthoritySlashes;
      } else if (RemainingStartsWith("/")) {
        state_ = State::PathOrAuthority;
        ++pointer_;
      } else {
        url_.opaquePath = "";
        state_ = State::OpaquePath;
      }
    } else if (!override_) {
      buffer_.clear();
      state_ = State::NoScheme;
      pointer_ = -1;
    } else {
      return false;
    }
    return true;
  }

  bool OnNoScheme() {
    int c = C();
    if (!base_ || (base_->opaquePath && c != '#')) return false;

    if (base_->opaquePath) {
      url_.scheme = base_->scheme;
      url_.opaquePath = base_->opaquePath;
      url_.query = base_->query;
      url_.fragment = "";
      state_ = State::Fragment;
    } else if (base_->scheme != "file") {
      state_ = State::Relative;
      --pointer_;
    } else {
      state_ = State::File;
      --pointer_;
    }
    return true;
  }

  bool OnSpecialRelativeOrAuthority() {
    if (C() == '/' && RemainingStartsWith("/")) {
      state_ = State::SpecialAuthorityIgnoreSlashes;
      ++pointer_;
    } else {
      state_ = State::Relative;
      --pointer_;
    }
    return true;
  }

  bool OnPathOrAuthority() {
    if (C() == '/') {
      state_ = State::Authority;
    } else {
      state_ = State::Path;
      --pointer_;
    }
    return true;
  }

  void CopyAuthorityFromBase() {
    url_.username = base_->username;
    url_.password = base_->password;
    url_.host = base_->host;
    url_.port = base_->port;
  }

  bool OnRelative() {
    int c = C();
    url_.scheme = base_->scheme;

    if (c == '/') {
      state_ = State::RelativeSlash;
    } else if (url_.IsSpecial() && c == '\\') {
      state_ = State::RelativeSlash;
    } else {
      CopyAuthorityFromBase();
      url_.path = base_->path;
      url_.query = base_->query;

      if (c == '?') {
        url_.query = "";
        state_ = State::Query;
      } else if (c == '#') {
        url_.fragment = "";
        state_ = State::Fragment;
      } else if (c != kEof) {
        url_.query.reset();
        ShortenPath(url_);
        state_ = State::Path;
        --pointer_;
      }
    }
    return true;
  }

  bool OnRelativeSlash() {
    int c = C();
    if (url_.IsSpecial() && (c == '/' || c == '\\')) {
      state_ = State::SpecialAuthorityIgnoreSlashes;
    } else if (c == '/') {
      state_ = State::Authority;
    } else {
      CopyAuthorityFromBase();
      state_ = State::Path;
      --pointer_;
    }
    return true;
  }

  bool OnSpecialAuthoritySlashes() {
    state_ = State::SpecialAuthorityIgnoreSlashes;
    if (C() == '/' && RemainingStartsWith("/")) {
      ++pointer_;
    } else {
      --pointer_;
    }
    return true;
  }

  bool OnSpecialAuthorityIgnoreSlashes() {
    int c = C();
    if (c != '/' && c != '\\') {
      state_ = State::Authority;
      --pointer_;
    }
    return true;
  }

  bool OnAuthority() {
    int c = C();
    if (c == '@') {
      if (atSignSeen_) buffer_.insert(0, "%40");
      atSignSeen_ = true;

      for (char byte : buffer_) {
        if (byte == ':' && !passwordTokenSeen_) {
          passwordTokenSeen_ = true;
          continue;
        }
        AppendPercentEncoded(passwordTokenSeen_ ? url_.password : url_.username,
                             static_cast<uint8_t>(byte), EncodeSet::Userinfo);
      }
      buffer_.clear();
    } else if (IsSpecialTerminator(c)) {
      if (atSignSeen_ && buffer_.empty()) return false;
      pointer_ -= static_cast<std::ptrdiff_t>(buffer_.size()) + 1;
      buffer_.clear();
      state_ = State::Host;
    } else {
      buffer_.push_back(static_cast<char>(c));
    }
    return true;
  }

  bool OnHost() {
    int c = C();
    if (override_ && url_.scheme == "file") {
      --pointer_;
      state_ = State::FileHost;
    } else if (c == ':' && !insideBrackets_) {
      if (buffer_.empty()) return false;
      if (override_ == State::Hostname) return false;
      std::optional<std::string> host = ParseHost(buffer_, !url_.IsSpecial());
      if (!host) return false;
      url_.host = std::move(host);
      buffer_.clear();
      state_ = State::Port;
    } else if (IsSpecialTerminator(c)) {
      --pointer_;
      if (url_.IsSpecial() && buffer_.empty()) return false;
      if (override_ && buffer_.empty() && (url_.IncludesCredentials() || url_.port)) return false;
      std::optional<std::string> host = ParseHost(buffer_, !url_.IsSpecial());
      if (!host) return false;
      url_.host = std::move(host);
      buffer_.clear();
      state_ = State::PathStart;
      if (override_) return Finish();
    } else {
      if (c == '[') insideBrackets_ = true;
      if (c == ']') insideBrackets_ = false;
      buffer_.push_back(static_cast<char>(c));
    }
    return true;
  }

  bool OnPort() {
    int c = C();
    if (IsAsciiDigit(c)) {
      buffer_.push_back(static_cast<char>(c));
    } else if (IsSpecialTerminator(c) || override_) {
      if (!buffer_.empty()) {
        uint32_t port = 0;
        for (char digit : buffer_) {
          port = port * 10 + (digit - '0');
          if (port > 0xFFFF) return false;
        }
        std::optional<uint16_t> defaultPort = DefaultPort(url_.scheme);
        if (defaultPort && *defaultPort == port) {
          url_.port.reset();
        } else {
          url_.port = static_cast<uint16_t>(port);
        }
        buffer_.clear();
        if (override_) return Finish();
      }
      if (override_) return false;
      state_ = State::PathStart;
      --pointer_;
    } else {
      return false;
    }
    return true;
  }

  bool OnFile() {
    int c = C();
    url_.scheme = "file";
    url_.host = "";

    if (c == '/' || c == '\\') {
      state_ = State::FileSlash;
    } else if (base_ && base_->scheme == "file") {
      url_.host = base_->host;
      url_.path = base_->path;
      url_.query = base_->query;

      if (c == '?') {
        url_.query = "";
        state_ = State::Query;
      } else if (c == '#') {
        url_.fragment = "";
        state_ = State::Fragment;
      } else if (c != kEof) {
        url_.query.reset();
        if (!StartsWithWindowsDriveLetter(input_.substr(pointer_))) {
          ShortenPath(url_);
        } else {
          url_.path.clear();
        }
        state_ = State::Path;
        --pointer_;
      }
    } else {
      state_ = State::Path;
      --pointer_;
    }
    return true;
  }

  bool OnFileSlash() {
    int c = C();
    if (c == '/' || c == '\\') {
      state_ = State::FileHost;
      return true;
    }

    if (base_ && base_->scheme == "file") {
      url_.host = base_->host;
      if (!StartsWithWindowsDriveLetter(input_.substr(pointer_)) && !base_->path.empty() &&
          IsNormalizedWindowsDriveLetter(base_->path[0])) {
        url_.path.push_back(base_->path[0]);
      }
    }
    state_ = State::Path;
    --pointer_;
    return true;
  }

  bool OnFileHost() {
    int c = C();
    if (c == kEof || c == '/' || c == '\\' || c == '?' || c == '#') {
      --pointer_;
      if (!override_ && IsWindowsDriveLetter(buffer_)) {
        // buffer_ is deliberately kept: the path state consumes it as the first segment.
        state_ = State::Path;
      } else if (buffer_.empty()) {
        url_.host = "";
        if (override_) return Finish();
        state_ = State::PathStart;
      } else {
        std::optional<std::string> host = ParseHost(buffer_, !url_.IsSpecial());
        if (!host) return false;
        if (*host == "localhost") host = "";
        url_.host = std::move(host);
        buffer_.clear();
        state_ = State::PathStart;
        if (override_) return Finish();
      }
    } else {
      buffer_.push_back(static_cast<char>(c));
    }
    return true;
  }

  bool OnPathStart() {
    int c = C();
    if (url_.IsSpecial()) {
      state_ = State::Path;
      if (c != '/' && c != '\\') --pointer_;
    } else if (!override_ && c == '?') {
      url_.query = "";
      state_ = State::Query;
    } else if (!override_ && c == '#') {
      url_.fragment = "";
      state_ = State::Fragment;
    } else if (c != kEof) {
      state_ = State::Path;
      if (c != '/') --pointer_;
    } else if (override_ && !url_.host) {
      url_.path.emplace_back();
    }
    return true;
  }

  bool OnPath() {
    int c = C();
    bool slash = c == '/' || (url_.IsSpecial() && c == '\\');

    if (c == kEof || slash || (!override_ && (c == '?' || c == '#'))) {
      if (IsDoubleDotSegment(buffer_)) {
        ShortenPath(url_);
        if (!slash) url_.path.emplace_back();
      } else if (IsSingleDotSegment(buffer_)) {
        if (!slash) url_.path.emplace_back();
      } else {
        if (url_.scheme == "file" && url_.path.empty() && IsWindowsDriveLetter(buffer_)) {
          buffer_[1] = ':';
        }
        url_.path.push_back(std::move(buffer_));
      }
      buffer_.clear();

      if (c == '?') {
        url_.query = "";
        state_ = State::Query;
      }
      if (c == '#') {
        url_.fragment = "";
        state_ = State::Fragment;
      }
    } else {
      AppendPercentEncoded(buffer_, static_cast<uint8_t>(c), EncodeSet::Path);
    }
    return true;
  }

  bool OnOpaquePath() {
    int c = C();
    if (c == '?') {
      url_.query = "";
      state_ = State::Query;
    } else if (c == '#') {
      url_.fragment = "";
      state_ = State::Fragment;
    } else if (c == ' ') {
      if (RemainingStartsWith("?") || RemainingStartsWith("#")) {
        *url_.opaquePath += "%20";
      } else {
        *url_.opaquePath += ' ';
      }
    } else if (c != kEof) {
      AppendPercentEncoded(*url_.opaquePath, static_cast<uint8_t>(c), EncodeSet::C0Control);
    }
    return true;
  }

  bool OnQuery() {
    int c = C();
    if ((!override_ && c == '#') || c == kEof) {
      EncodeSet set = url_.IsSpecial() ? EncodeSet::SpecialQuery : EncodeSet::Query;
      AppendPercentEncoded(*url_.query, buffer_, set);
      buffer_.clear();
      if (c == '#') {
        url_.fragment = "";
        state_ = State::Fragment;
      }
    } else {
      buffer_.push_back(static_cast<char>(c));
    }
    return true;
  }

  bool OnFragment() {
    int c = C();
    if (c != kEof) {
      AppendPercentEncoded(*url_.fragment, static_cast<uint8_t>(c), EncodeSet::Fragment);
    }
    return true;
  }

  // Ends a state-override run successfully, which the standard words as a bare "return".
  bool Finish() {
    done_ = true;
    return true;
  }

  std::string_view input_;
  const Url* base_;
  Url& url_;
  std::optional<State> override_;
  State state_;
  bool done_ = false;
  std::string buffer_;
  bool atSignSeen_ = false;
  bool insideBrackets_ = false;
  bool passwordTokenSeen_ = false;
  std::ptrdiff_t pointer_ = 0;
};

std::string Sanitize(std::string_view input, bool trimControlsAndSpace) {
  std::string scalarValues;
  scalarValues.reserve(input.size());
  for (char32_t codePoint : DecodeUtf8(input)) AppendUtf8(scalarValues, codePoint);

  size_t begin = 0;
  size_t end = scalarValues.size();
  if (trimControlsAndSpace) {
    while (begin < end && static_cast<unsigned char>(scalarValues[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(scalarValues[end - 1]) <= 0x20) --end;
  }

  std::string out;
  out.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    char c = scalarValues[i];
    if (c != '\t' && c != '\n' && c != '\r') out.push_back(c);
  }
  return out;
}

}  // namespace

std::optional<Url> Parse(std::string_view input, const Url* base) {
  std::string sanitized = Sanitize(input, true);
  Url url;
  if (!BasicUrlParser(sanitized, base, url, std::nullopt).Run()) return std::nullopt;
  return url;
}

bool ParseInto(Url& url, std::string_view input, StateOverride state) {
  static constexpr State kStates[] = {State::SchemeStart, State::Host,      State::Hostname,
                                      State::Port,        State::PathStart, State::Query,
                                      State::Fragment};
  std::string sanitized = Sanitize(input, false);
  return BasicUrlParser(sanitized, nullptr, url, kStates[static_cast<int>(state)]).Run();
}

}  // namespace solar::url
