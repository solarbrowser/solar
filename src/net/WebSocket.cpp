/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/WebSocket.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <map>
#include <unordered_map>

#include "solar/net/ConnectRace.h"
#include "solar/net/Cookies.h"
#include "solar/url/Origin.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"

namespace solar::net {

namespace {

constexpr const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t kMaxHandshakeBytes = 64 * 1024;
constexpr std::chrono::milliseconds kCloseWait{5000};

std::string Base64(const uint8_t* data, size_t size) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t n = (uint32_t{data[i]} << 16) | (i + 1 < size ? uint32_t{data[i + 1]} << 8 : 0) | (i + 2 < size ? uint32_t{data[i + 2]} : 0);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += i + 1 < size ? alphabet[(n >> 6) & 63] : '=';
    out += i + 2 < size ? alphabet[n & 63] : '=';
  }
  return out;
}

std::string Sha1(const std::string& text) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_Digest(text.data(), text.size(), digest, &length, EVP_sha1(), nullptr);
  return std::string(reinterpret_cast<const char*>(digest), length);
}

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string Trim(const std::string& text) {
  size_t a = 0, b = text.size();
  while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
  while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
  return text.substr(a, b - a);
}

// Whether the bytes are well-formed UTF-8 (a text message must be).
bool ValidUtf8(const std::string& text) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t extra;
    uint32_t minimum, code;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xC2 && c <= 0xDF) { extra = 1; minimum = 0x80; code = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { extra = 2; minimum = 0x800; code = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { extra = 3; minimum = 0x10000; code = c & 0x07; }
    else return false;
    if (i + extra >= n + 0 && i + extra > n - 1) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char d = static_cast<unsigned char>(text[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      code = (code << 6) | (d & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

// The status codes a Close frame may carry (RFC 6455 7.4).
bool ValidCloseCode(uint16_t code) {
  if (code >= 3000 && code <= 4999) return true;
  switch (code) {
    case 1000: case 1001: case 1002: case 1003: case 1007: case 1008: case 1009: case 1010: case 1011: return true;
    default: return false;
  }
}

enum class State { Connecting, Handshake, Open, Closing, Closed };

}  // namespace

struct WebSocketClient::Impl {
  explicit Impl(HttpClient& h) : http(h), loop(h.loop()), resolver(h.resolver()) {}
  HttpClient& http;
  Loop& loop;
  std::shared_ptr<Resolver> resolver;
  uint64_t next = 1;
  struct Connection;
  std::unordered_map<uint64_t, std::unique_ptr<Connection>> connections;
  std::vector<uint64_t> finished;
  bool reapQueued = false;
  bool closingAll = false;
  void Reap();
};

struct WebSocketClient::Impl::Connection final : ConnectionHandler {
  Connection(Impl& i, uint64_t id_, url::Url u, WebSocketHandler& h, WebSocketOptions o) : impl(i), id(id_), url(std::move(u)), handler(h), options(std::move(o)) {}
  Impl& impl;
  uint64_t id;
  url::Url url;
  WebSocketHandler& handler;
  WebSocketOptions options;

  State state = State::Connecting;
  std::unique_ptr<internal::ConnectRace> race;
  Connection* dummy = nullptr;
  net::Connection* connection = nullptr;
  std::unique_ptr<TlsLayer> tls;
  Transport* transport = nullptr;
  std::string key;
  std::string pending;  // what has come and has not been made into a handshake or frames yet
  std::string message;  // the message being put together from fragments
  bool messageBinary = false;
  bool inMessage = false;
  bool messageCompressed = false;
  // permessage-deflate (RFC 7692), when the server agreed to it.
  bool deflating = false, deflaterReady = false, inflaterReady = false;
  bool clientNoTakeover = false, serverNoTakeover = false;
  int clientBits = 15, serverBits = 15;
  z_stream deflater{}, inflater{};
  ~Connection() override {
    if (closeTimer) impl.loop.CancelTimer(closeTimer);
    if (deflaterReady) deflateEnd(&deflater);
    if (inflaterReady) inflateEnd(&inflater);
  }
  // How it ended, to be told when the transport is closed.
  bool closeReceived = false;
  bool closeSent = false;
  uint16_t closeCode = 1006;
  std::string closeReason;
  std::string failure;
  bool told = false;
  Loop::TimerId closeTimer = 0;
  size_t buffered = 0;

  void Begin() {
    std::string name = url.host.value_or("");
    if (name.size() >= 2 && name.front() == '[') name = name.substr(1, name.size() - 2);
    const bool secure = url.scheme == "wss";
    const uint16_t port = url.port.value_or(secure ? 443 : 80);
    race = std::make_unique<internal::ConnectRace>(
        impl.loop, *impl.resolver, std::move(name), port, internal::AddressRace::Config{}, [this](net::Connection* winner) { OnRaceWon(winner); },
        [this](const std::string& message_) { OnRaceFailed(message_); });
    race->Start();
  }

  void OnRaceWon(net::Connection* winner) {
    connection = winner;
    if (state == State::Closed) {
      winner->SetHandler(this);
      winner->Close();
      return;
    }
    if (url.scheme != "wss") {
      winner->SetHandler(this);
      transport = winner;
      OnConnected();
      return;
    }
    tls = std::make_unique<TlsLayer>(options.tls ? options.tls : DefaultTlsContext(), url.host.value_or(""), *this);
    winner->SetHandler(tls.get());
    tls->Attach(winner);
    transport = tls.get();
    tls->OnConnected();  // the handshake starts now; OnConnected reaches this when it is done
  }

  void OnRaceFailed(const std::string& message_) {
    failure = message_;
    state = State::Closed;
    Tell();
  }

  void OnConnected() override {
    state = State::Handshake;
    uint8_t random[16];
    RAND_bytes(random, sizeof random);
    key = Base64(random, sizeof random);
    url::Url forCookies = url;
    forCookies.scheme = url.scheme == "wss" ? "https" : "http";
    std::string target = url::SerializePath(url);
    if (url.query) target += "?" + *url.query;
    std::string request = "GET " + target + " HTTP/1.1\r\nHost: " + url::GetHost(forCookies) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n";
    if (!options.origin.empty()) request += "Origin: " + options.origin + "\r\n";
    request += "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";
    if (!options.protocols.empty()) {
      request += "Sec-WebSocket-Protocol: ";
      for (size_t i = 0; i < options.protocols.size(); ++i) request += (i ? ", " : "") + options.protocols[i];
      request += "\r\n";
    }
    bool hasUserAgent = false;
    for (const auto& [name, value] : options.headers) {
      if (Lower(name) == "user-agent") hasUserAgent = true;
      request += name + ": " + value + "\r\n";
    }
    if (!hasUserAgent) request += "User-Agent: " + impl.http.userAgent() + "\r\n";
    request += "Pragma: no-cache\r\nCache-Control: no-cache\r\n";
    CookieRequest cookieRequest;
    if (options.initiator) {
      cookieRequest.sameSite = url::SerializeOrigin(*options.initiator) == url::SerializeOrigin(forCookies);
    }
    const std::string cookies = impl.http.cookies()->HeaderFor(forCookies, cookieRequest);
    if (!cookies.empty()) request += "Cookie: " + cookies + "\r\n";
    request += "\r\n";
    transport->Send(std::move(request));
  }

  void OnData(std::span<const uint8_t> data) override {
    if (state == State::Closed) return;
    pending.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (state == State::Handshake) {
      if (!Handshake()) return;
    }
    if (state == State::Open || state == State::Closing) ReadFrames();
  }

  // The upgrade response: true once it was taken and accepted.
  bool Handshake() {
    const size_t end = pending.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (pending.size() > kMaxHandshakeBytes) Fail("the server's response is too long");
      return false;
    }
    const std::string head = pending.substr(0, end);
    pending.erase(0, end + 4);
    size_t line = head.find("\r\n");
    const std::string status = head.substr(0, line);
    std::map<std::string, std::string> fields;
    std::vector<std::pair<std::string, std::string>> cookiesSet;
    while (line != std::string::npos) {
      const size_t from = line + 2;
      line = head.find("\r\n", from);
      const std::string text = head.substr(from, line == std::string::npos ? std::string::npos : line - from);
      const size_t colon = text.find(':');
      if (colon == std::string::npos) continue;
      const std::string name = Lower(Trim(text.substr(0, colon)));
      const std::string value = Trim(text.substr(colon + 1));
      if (name == "set-cookie") cookiesSet.emplace_back(name, value);
      auto [at, inserted] = fields.emplace(name, value);
      if (!inserted) at->second += ", " + value;
    }
    if (status.rfind("HTTP/1.1 101", 0) != 0) {
      Fail("the server did not upgrade the connection (" + status + ")");
      return false;
    }
    if (Lower(fields["upgrade"]) != "websocket") { Fail("the response has no 'Upgrade: websocket'"); return false; }
    {
      // Connection names upgrade among a list.
      std::string tokens = Lower(fields["connection"]);
      if (tokens.find("upgrade") == std::string::npos) { Fail("the response has no 'Connection: Upgrade'"); return false; }
    }
    const std::string expected = Base64(reinterpret_cast<const uint8_t*>(Sha1(key + kGuid).data()), 20);
    if (fields["sec-websocket-accept"] != expected) { Fail("the Sec-WebSocket-Accept of the response is not the one for the key"); return false; }
    std::string extensions;
    if (fields.count("sec-websocket-extensions")) {
      if (!AcceptExtensions(fields["sec-websocket-extensions"])) { Fail("the server chose an extension that was not offered, or parameters that are not allowed"); return false; }
      extensions = "permessage-deflate";
    }
    const std::string protocol = fields["sec-websocket-protocol"];
    if (!protocol.empty()) {
      if (std::find(options.protocols.begin(), options.protocols.end(), protocol) == options.protocols.end()) { Fail("the server chose a sub-protocol that was not offered"); return false; }
    } else if (!options.protocols.empty()) {
      // (A server that names none while some were offered is taken to have none: the page sees an empty protocol.)
    }
    // Cookies the server set are the jar's.
    url::Url forCookies = url;
    forCookies.scheme = url.scheme == "wss" ? "https" : "http";
    for (const auto& [name, value] : cookiesSet) impl.http.cookies()->Store(forCookies, value, CookieRequest{});
    state = State::Open;
    handler.OnOpen(protocol, extensions);
    return true;
  }

  // The server's Sec-WebSocket-Extensions: only permessage-deflate was offered, and only once.
  bool AcceptExtensions(const std::string& value) {
    if (value.find(',') != std::string::npos) return false;
    size_t at = 0;
    bool first = true;
    while (at <= value.size()) {
      size_t end = value.find(';', at);
      if (end == std::string::npos) end = value.size();
      std::string token = Lower(Trim(value.substr(at, end - at)));
      at = end + 1;
      if (first) {
        first = false;
        if (token != "permessage-deflate") return false;
        continue;
      }
      std::string name = token, argument;
      if (const size_t eq = token.find('='); eq != std::string::npos) {
        name = Trim(token.substr(0, eq));
        argument = Trim(token.substr(eq + 1));
        if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"') argument = argument.substr(1, argument.size() - 2);
      }
      auto bits = [&](int& out) {
        if (argument.empty() || argument.size() > 2 || !std::isdigit(static_cast<unsigned char>(argument[0]))) return false;
        out = std::atoi(argument.c_str());
        return out >= 8 && out <= 15;
      };
      if (name == "server_no_context_takeover") serverNoTakeover = true;
      else if (name == "client_no_context_takeover") clientNoTakeover = true;
      else if (name == "server_max_window_bits") { if (!bits(serverBits)) return false; }
      else if (name == "client_max_window_bits") { if (!bits(clientBits)) return false; }
      else return false;
      if (end == value.size()) break;
    }
    // (zlib's raw streams do not take a window of 8: 9 holds the same.)
    if (deflateInit2(&deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -std::max(9, clientBits), 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    deflaterReady = true;
    if (inflateInit2(&inflater, -std::max(9, serverBits)) != Z_OK) return false;
    inflaterReady = true;
    deflating = true;
    return true;
  }

  // A message as it goes: deflated, without the empty block that ends it.
  bool Compress(const std::string& in, std::string& out) {
    deflater.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    deflater.avail_in = static_cast<uInt>(in.size());
    uint8_t buffer[16384];
    do {
      deflater.next_out = buffer;
      deflater.avail_out = sizeof buffer;
      if (deflate(&deflater, Z_SYNC_FLUSH) == Z_STREAM_ERROR) return false;
      out.append(reinterpret_cast<char*>(buffer), sizeof buffer - deflater.avail_out);
    } while (deflater.avail_out == 0);
    if (out.size() < 4 || out.compare(out.size() - 4, 4, std::string("\x00\x00\xff\xff", 4)) != 0) return false;
    out.resize(out.size() - 4);
    if (clientNoTakeover) deflateReset(&deflater);
    return true;
  }

  bool Decompress(const std::string& in, std::string& out) {
    std::string input = in;
    input.append("\x00\x00\xff\xff", 4);
    inflater.next_in = reinterpret_cast<Bytef*>(input.data());
    inflater.avail_in = static_cast<uInt>(input.size());
    uint8_t buffer[16384];
    for (;;) {
      inflater.next_out = buffer;
      inflater.avail_out = sizeof buffer;
      const int status = inflate(&inflater, Z_SYNC_FLUSH);
      if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) return false;
      out.append(reinterpret_cast<char*>(buffer), sizeof buffer - inflater.avail_out);
      if (out.size() > options.maxMessageBytes) return false;
      if (inflater.avail_out != 0 || status == Z_STREAM_END) break;
    }
    if (serverNoTakeover) inflateReset(&inflater);
    return true;
  }

  // Frames out of `pending`, as many as there are.
  void ReadFrames() {
    while (state == State::Open || state == State::Closing) {
      if (pending.size() < 2) return;
      const uint8_t b0 = static_cast<uint8_t>(pending[0]), b1 = static_cast<uint8_t>(pending[1]);
      const bool fin = b0 & 0x80;
      const int reserved = (b0 >> 4) & 7;
      const int opcode = b0 & 15;
      const bool masked = b1 & 0x80;
      uint64_t length = b1 & 0x7F;
      size_t header = 2;
      const bool compressed = (reserved & 4) != 0;
      if ((reserved & 3) || (compressed && (!deflating || opcode == 0 || opcode >= 8))) { Fail("a frame has a reserved bit set", 1002); return; }
      if (masked) { Fail("a frame from the server is masked", 1002); return; }
      if (length == 126) {
        if (pending.size() < 4) return;
        length = (uint64_t{static_cast<uint8_t>(pending[2])} << 8) | static_cast<uint8_t>(pending[3]);
        header = 4;
      } else if (length == 127) {
        if (pending.size() < 10) return;
        length = 0;
        for (int i = 0; i < 8; ++i) length = (length << 8) | static_cast<uint8_t>(pending[2 + i]);
        if (length >> 63) { Fail("a frame's length is not a length", 1002); return; }
        header = 10;
      }
      const bool control = opcode >= 8;
      if (control && (!fin || length > 125)) { Fail("a control frame is fragmented or too long", 1002); return; }
      if (!control && length > options.maxMessageBytes) { Fail("a message is too big", 1009); return; }
      if (pending.size() < header + length) return;
      const std::string payload = pending.substr(header, static_cast<size_t>(length));
      pending.erase(0, header + static_cast<size_t>(length));
      if (!Frame(fin, opcode, payload, compressed)) return;
    }
  }

  bool Frame(bool fin, int opcode, const std::string& payload, bool compressed) {
    switch (opcode) {
      case 0:  // continuation
        if (!inMessage) { Fail("a continuation with nothing to continue", 1002); return false; }
        message += payload;
        break;
      case 1:
      case 2:
        if (inMessage) { Fail("a new message while another is not done", 1002); return false; }
        inMessage = true;
        messageCompressed = compressed;
        messageBinary = opcode == 2;
        message = payload;
        break;
      case 8: {
        uint16_t code = 1005;
        std::string reason;
        if (payload.size() == 1) { Fail("a Close frame has one byte", 1002); return false; }
        if (payload.size() >= 2) {
          code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]));
          reason = payload.substr(2);
          if (!ValidCloseCode(code) || !ValidUtf8(reason)) { Fail("a Close frame has a code or reason that is not valid", 1002); return false; }
        }
        closeReceived = true;
        closeCode = code;
        closeReason = reason;
        if (!closeSent) {
          closeSent = true;
          std::string answer;
          if (code != 1005) { answer += static_cast<char>(code >> 8); answer += static_cast<char>(code & 0xFF); }
          SendFrame(8, std::move(answer));
        }
        state = State::Closing;
        // The server ends the TCP connection after the close handshake; this does as well, a moment after, if it does not.
        transport->Close();
        return false;
      }
      case 9:
        SendFrame(10, payload);
        return true;
      case 10:
        return true;
      default:
        Fail("a frame has an opcode that there is none for", 1002);
        return false;
    }
    if (!fin) {
      if (message.size() > options.maxMessageBytes) { Fail("a message is too big", 1009); return false; }
      return true;
    }
    inMessage = false;
    if (messageCompressed) {
      std::string plain;
      if (!Decompress(message, plain)) { Fail("a compressed message could not be inflated", 1007); return false; }
      message = std::move(plain);
    }
    if (!messageBinary && !ValidUtf8(message)) { Fail("a text message is not UTF-8", 1007); return false; }
    std::string whole = std::move(message);
    message.clear();
    if (state == State::Open) handler.OnMessage(std::move(whole), messageBinary);
    return state == State::Open || state == State::Closing;
  }

  void SendFrame(int opcode, std::string payload, bool rsv1 = false) {
    if (!transport || state == State::Closed) return;
    std::string frame;
    frame += static_cast<char>(0x80 | (rsv1 ? 0x40 : 0) | opcode);
    const size_t n = payload.size();
    if (n < 126) {
      frame += static_cast<char>(0x80 | n);
    } else if (n <= 0xFFFF) {
      frame += static_cast<char>(0x80 | 126);
      frame += static_cast<char>(n >> 8);
      frame += static_cast<char>(n & 0xFF);
    } else {
      frame += static_cast<char>(0x80 | 127);
      for (int i = 7; i >= 0; --i) frame += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
    }
    uint8_t mask[4];
    RAND_bytes(mask, sizeof mask);
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < n; ++i) frame += static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
    transport->Send(std::move(frame));
  }

  void Send(std::string data, bool binary) {
    if (state != State::Open) return;
    if (deflating && data.size() >= 32) {  // (a short message costs more to deflate than it saves)
      std::string packed;
      if (Compress(data, packed)) { SendFrame(binary ? 2 : 1, std::move(packed), true); return; }
    }
    SendFrame(binary ? 2 : 1, std::move(data));
  }

  void Close(uint16_t code, std::string reason) {
    if (state == State::Connecting || state == State::Handshake) {
      // Not open: the connection is dropped, and the page sees it fail.
      failure = "the connection was closed before it was open";
      Drop();
      return;
    }
    if (state != State::Open || closeSent) return;
    closeSent = true;
    state = State::Closing;
    closeCode = code == 1005 ? 1005 : code;
    closeReason = reason;
    std::string payload;
    if (code != 1005) {
      payload += static_cast<char>(code >> 8);
      payload += static_cast<char>(code & 0xFF);
      payload += reason;
    }
    SendFrame(8, std::move(payload));
    closeTimer = impl.loop.PostDelayed(kCloseWait, [this] {
      closeTimer = 0;
      if (transport && state != State::Closed) transport->Close();
    });
  }

  // The connection failed: a Close frame with the code if there is a way to say it, and the transport closed.
  void Fail(const std::string& why, uint16_t code = 1002) {
    if (state == State::Closed) return;
    failure = why;
    if (state == State::Open && !closeSent) {
      closeSent = true;
      std::string payload;
      payload += static_cast<char>(code >> 8);
      payload += static_cast<char>(code & 0xFF);
      SendFrame(8, std::move(payload));
    }
    state = State::Closing;
    if (transport) transport->Close();
  }

  // Ends the connection where it is (no closing handshake).
  void Drop() {
    if (state == State::Closed) return;
    if (!transport) {
      // (Still being made: what comes of the race is ignored.)
      state = State::Closed;
      race.reset();
      Tell();
      return;
    }
    state = State::Closing;
    transport->Close();
  }

  void OnClosed(int error) override {
    transport = nullptr;
    connection = nullptr;
    state = State::Closed;
    if (failure.empty() && !closeReceived && error != 0) failure = std::strerror(error);
    if (failure.empty() && tls && !tls->failure().empty()) failure = tls->failure();
    Tell();
  }

  // The end, told once and from the loop (never from inside what asked for it).
  void Tell() {
    if (told) return;
    told = true;
    const bool clean = closeReceived && closeSent && failure.empty();
    const uint16_t code = clean ? closeCode : 1006;
    const std::string reason = clean ? closeReason : std::string();
    impl.loop.Post([this, clean, code, reason, why = failure] {
      handler.OnClosed(clean, code, reason, why);
      impl.finished.push_back(id);
      impl.Reap();
    });
  }
};

void WebSocketClient::Impl::Reap() {
  if (reapQueued) return;
  reapQueued = true;
  loop.Post([this] {
    reapQueued = false;
    std::vector<uint64_t> done = std::move(finished);
    finished.clear();
    for (uint64_t id : done) {
      const auto found = connections.find(id);
      // (A connection whose transport has not finished closing is kept until it has.)
      if (found != connections.end() && found->second->state == State::Closed && !found->second->transport) connections.erase(found);
    }
  });
}

WebSocketClient::WebSocketClient(HttpClient& http) : impl_(std::make_unique<Impl>(http)) {}

WebSocketClient::~WebSocketClient() = default;

WebSocketHandle WebSocketClient::Connect(const url::Url& address, WebSocketHandler& handler, WebSocketOptions options) {
  url::Url url = address;
  if (url.scheme == "http") url.scheme = "ws";
  if (url.scheme == "https") url.scheme = "wss";
  const uint64_t id = impl_->next++;
  auto connection = std::make_unique<Impl::Connection>(*impl_, id, url, handler, std::move(options));
  Impl::Connection* raw = connection.get();
  impl_->connections[id] = std::move(connection);
  if ((url.scheme != "ws" && url.scheme != "wss") || !url.host || url.host->empty()) {
    raw->failure = "the address is not a WebSocket one";
    raw->state = State::Closed;
    raw->Tell();
    return WebSocketHandle(this, id);
  }
  // Begun from the loop: nothing is reported from inside this call.
  impl_->loop.Post([this, id] {
    const auto found = impl_->connections.find(id);
    if (found != impl_->connections.end() && found->second->state == State::Connecting) found->second->Begin();
  });
  return WebSocketHandle(this, id);
}

void WebSocketClient::CloseAll() {
  for (auto& [id, connection] : impl_->connections) {
    if (connection->state == State::Closed) continue;
    connection->failure = connection->failure.empty() ? "the page went away" : connection->failure;
    connection->Drop();
  }
}

void WebSocketHandle::Send(std::string data, bool binary) {
  if (!client_) return;
  const auto found = client_->impl_->connections.find(id_);
  if (found != client_->impl_->connections.end()) found->second->Send(std::move(data), binary);
}

void WebSocketHandle::Close(uint16_t code, std::string reason) {
  if (!client_) return;
  const auto found = client_->impl_->connections.find(id_);
  if (found != client_->impl_->connections.end()) found->second->Close(code, std::move(reason));
}

size_t WebSocketHandle::BufferedAmount() const { return 0; }

}  // namespace solar::net
