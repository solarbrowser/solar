#include "Http2TestServer.h"

#include "solar/net/Nghttp2Compat.h"
#include <openssl/ssl.h>

#include <algorithm>
#include <map>
#include <thread>

#include "SocketCompat.h"

namespace solar::test {

namespace {

// A response on its way out: the body is read from here as the flow-control windows allow.
struct Outgoing {
  std::string body;
  size_t sent = 0;
  Fields trailers;
  std::optional<size_t> resetAfter;
  std::chrono::milliseconds pace{0};
  std::chrono::steady_clock::time_point nextPiece{};
};

struct Waiting {
  int32_t stream;
  H2Reply reply;
  std::chrono::steady_clock::time_point due;
};

// Views, not strings: a literal passed here must not become a temporary the field then points into.
nghttp2_nv Field(std::string_view name, std::string_view value) {
  return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(),
          value.size(), NGHTTP2_NV_FLAG_NONE};
}

struct Session {
  Http2TestServer::Handler handler;
  std::function<void(const H2Request&)> record;
  std::atomic<int>* maxConcurrent;
  std::atomic<int>* resets;
  nghttp2_session* session = nullptr;
  std::map<int32_t, H2Request> requests;
  std::map<int32_t, std::unique_ptr<Outgoing>> outgoing;
  std::vector<Waiting> waiting;
  int open = 0;
  int32_t lastProcessed = 0;
  bool stop = false;
};

nghttp2_ssize ReadBody(nghttp2_session*, int32_t, uint8_t* buffer, size_t length, uint32_t* flags, nghttp2_data_source* source, void*) {
  auto* out = static_cast<Outgoing*>(source->ptr);
  if (out->resetAfter && out->sent >= *out->resetAfter) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  if (out->pace.count() > 0) {
    if (std::chrono::steady_clock::now() < out->nextPiece) return NGHTTP2_ERR_DEFERRED;
    length = std::min<size_t>(length, 4096);
    out->nextPiece = std::chrono::steady_clock::now() + out->pace;
  }
  size_t take = std::min(length, out->body.size() - out->sent);
  if (out->resetAfter) take = std::min(take, *out->resetAfter - out->sent);
  std::copy_n(out->body.data() + out->sent, take, buffer);
  out->sent += take;
  if (out->sent >= out->body.size()) {
    *flags |= NGHTTP2_DATA_FLAG_EOF;
    if (!out->trailers.empty()) *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
  }
  return static_cast<nghttp2_ssize>(take);
}

void Answer(Session& s, int32_t stream, const H2Reply& reply) {
  if (reply.closeConnection) {
    s.stop = true;
    return;
  }
  if (reply.goAwayInstead) {
    nghttp2_submit_goaway(s.session, NGHTTP2_FLAG_NONE, s.lastProcessed, NGHTTP2_NO_ERROR, nullptr, 0);
    return;
  }
  for (int status : reply.interim) {
    const std::string text = std::to_string(status);
    const nghttp2_nv header = Field(":status", text);
    nghttp2_submit_headers(s.session, NGHTTP2_FLAG_NONE, stream, nullptr, &header, 1, nullptr);
  }

  const std::string status = std::to_string(reply.status);
  const std::string length = std::to_string(reply.contentLength.value_or(reply.body.size()));
  std::vector<nghttp2_nv> fields = {Field(":status", status), Field("content-length", length)};
  for (const auto& [name, value] : reply.headers) fields.push_back(Field(name, value));

  auto out = std::make_unique<Outgoing>();
  out->body = reply.body;
  out->trailers = reply.trailers;
  out->resetAfter = reply.resetAfter;
  out->pace = reply.pace;
  nghttp2_data_provider2 provider{};
  provider.source.ptr = out.get();
  provider.read_callback = ReadBody;
  s.outgoing[stream] = std::move(out);
  nghttp2_submit_response2(s.session, stream, fields.data(), fields.size(), &provider);
}

// Trailers go out once the body has: find the streams whose body is done and say so.
void SendTrailers(Session& s) {
  for (auto& [stream, out] : s.outgoing) {
    if (out->trailers.empty() || out->sent < out->body.size()) continue;
    std::vector<nghttp2_nv> fields;
    for (const auto& [name, value] : out->trailers) fields.push_back(Field(name, value));
    nghttp2_submit_trailer(s.session, stream, fields.data(), fields.size());
    out->trailers.clear();
  }
}

int OnBeginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* user) {
  auto* s = static_cast<Session*>(user);
  if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) s->requests[frame->hd.stream_id] = {};
  return 0;
}

int OnHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength, const uint8_t* value, size_t valueLength,
             uint8_t, void* user) {
  auto* s = static_cast<Session*>(user);
  auto it = s->requests.find(frame->hd.stream_id);
  if (it == s->requests.end()) return 0;
  const std::string key(reinterpret_cast<const char*>(name), nameLength);
  const std::string text(reinterpret_cast<const char*>(value), valueLength);
  if (key == ":method") it->second.method = text;
  else if (key == ":path") it->second.path = text;
  else if (key == ":authority") it->second.authority = text;
  else if (key == ":scheme") it->second.scheme = text;
  else if (key[0] != ':') it->second.headers.emplace_back(key, text);
  return 0;
}

// Answers a request once all of it has come.
void Dispatch(Session* s, int32_t stream) {
  H2Request& request = s->requests[stream];
  s->record(request);
  ++s->open;
  int seen = s->maxConcurrent->load();
  while (s->open > seen && !s->maxConcurrent->compare_exchange_weak(seen, s->open)) {
  }
  H2Reply reply = s->handler(request);
  if (!reply.goAwayInstead) s->lastProcessed = stream;
  if (reply.delay.count() > 0) {
    const auto due = std::chrono::steady_clock::now() + reply.delay;
    s->waiting.push_back({stream, std::move(reply), due});
  } else {
    Answer(*s, stream, reply);
  }
}

int OnData(nghttp2_session*, uint8_t, int32_t stream, const uint8_t* data, size_t length, void* user) {
  auto* s = static_cast<Session*>(user);
  auto it = s->requests.find(stream);
  if (it != s->requests.end()) it->second.body.append(reinterpret_cast<const char*>(data), length);
  return 0;
}

int OnFrame(nghttp2_session*, const nghttp2_frame* frame, void* user) {
  auto* s = static_cast<Session*>(user);
  if (frame->hd.type == NGHTTP2_RST_STREAM) ++*s->resets;
  if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) return 0;
  const bool requestHeaders = frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST;
  if (requestHeaders || frame->hd.type == NGHTTP2_DATA) {
    if (s->requests.count(frame->hd.stream_id)) Dispatch(s, frame->hd.stream_id);
  }
  return 0;
}

int OnClose(nghttp2_session*, int32_t stream, uint32_t, void* user) {
  auto* s = static_cast<Session*>(user);
  if (s->requests.erase(stream)) --s->open;
  s->outgoing.erase(stream);
  return 0;
}

}  // namespace

Http2TestServer::Http2TestServer(const Identity& identity, Handler handler, H2ServerOptions options)
    : handler_(std::move(handler)), options_(options) {
  tls_ = std::make_unique<TlsTestServer>(identity, [this](ssl_st* connection) { Serve(connection); }, true, std::vector<std::string>{"h2"});
}

Http2TestServer::~Http2TestServer() = default;

std::vector<H2Request> Http2TestServer::requests() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return requests_;
}

void Http2TestServer::Serve(ssl_st* connection) {
  ++connections_;
  Session s;
  s.handler = handler_;
  s.record = [this](const H2Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(request);
  };
  s.maxConcurrent = &maxConcurrent_;
  s.resets = &resets_;

  nghttp2_session_callbacks* callbacks = nullptr;
  nghttp2_session_callbacks_new(&callbacks);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, OnBeginHeaders);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, OnHeader);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, OnFrame);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, OnData);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, OnClose);
  nghttp2_option* option = nullptr;
  nghttp2_option_new(&option);
  nghttp2_option_set_max_send_header_block_length(option, 4 << 20);  // so a test can send headers too big for the client
  nghttp2_session_server_new2(&s.session, callbacks, &s, option);
  nghttp2_option_del(option);
  nghttp2_session_callbacks_del(callbacks);

  const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, options_.maxConcurrentStreams}};
  nghttp2_submit_settings(s.session, NGHTTP2_FLAG_NONE, settings, 1);

  const sock::Handle socket = static_cast<sock::Handle>(SSL_get_fd(connection));
  uint8_t buffer[16384];
  while (!s.stop) {
    SendTrailers(s);
    // What is due is answered, and a body being paced gets its next piece.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.waiting.begin(); it != s.waiting.end();) {
      if (it->due <= now) {
        Answer(s, it->stream, it->reply);
        it = s.waiting.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& [stream, out] : s.outgoing) {
      if (out->pace.count() > 0 && out->sent < out->body.size()) nghttp2_session_resume_data(s.session, stream);
    }

    while (true) {
      const uint8_t* bytes = nullptr;
      const nghttp2_ssize length = nghttp2_session_mem_send2(s.session, &bytes);
      if (length <= 0) break;
      TlsTestServer::SendAll(connection, std::string_view(reinterpret_cast<const char*>(bytes), static_cast<size_t>(length)));
    }
    if (s.stop) break;
    if (!nghttp2_session_want_read(s.session) && !nghttp2_session_want_write(s.session)) break;

    if (SSL_pending(connection) == 0 && !sock::WaitReadable(socket, 5)) continue;
    const int got = SSL_read(connection, buffer, sizeof(buffer));
    if (got <= 0) break;
    if (nghttp2_session_mem_recv2(s.session, buffer, static_cast<size_t>(got)) < 0) break;
  }
  nghttp2_session_del(s.session);
}

}  // namespace solar::test
