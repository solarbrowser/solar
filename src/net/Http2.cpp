#include "Http2.h"

#include "Nghttp2Compat.h"

#include <algorithm>
#include <charconv>

namespace solar::net::internal {

namespace {

// What a response's header fields may add up to, counted as RFC 9113 does (each field is its
// name, its value and 32 bytes of overhead). It is also what the server is told in our settings.
constexpr size_t kMaxHeaderListBytes = 64 * 1024;
// A body is handed on as it arrives, so these only say how far a server may run ahead of us.
constexpr int32_t kStreamWindow = 1 << 20;
constexpr int32_t kConnectionWindow = 8 << 20;

nghttp2_nv Field(const std::string& name, const std::string& value) {
  return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(),
          value.size(), NGHTTP2_NV_FLAG_NONE};
}

}  // namespace

struct Http2Session::StreamState {
  Http2Stream* sink;  // null once the caller has lost interest
  HttpResponseHead head;
  size_t headerBytes = 0;
  bool headDelivered = false;
  bool ended = false;
  std::string failure;  // why this side gave up on the stream, if it did
};

// The functions nghttp2 calls. They are members only to reach Http2Session's private state.
struct Http2Session::Callbacks {
  static StreamState* StateOf(nghttp2_session* session, int32_t id) {
    return static_cast<StreamState*>(nghttp2_session_get_stream_user_data(session, id));
  }

  static int OnHeader(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t nameLength, const uint8_t* value,
                      size_t valueLength, uint8_t, void*) {
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    StreamState* state = StateOf(session, frame->hd.stream_id);
    if (!state || state->headDelivered) return 0;  // a trailer section has nowhere to go

    state->headerBytes += nameLength + valueLength + 32;
    if (state->headerBytes > kMaxHeaderListBytes) {
      state->failure = "the response headers are too large";
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;  // resets this stream and leaves the others be
    }
    const std::string_view field(reinterpret_cast<const char*>(name), nameLength);
    const std::string_view text(reinterpret_cast<const char*>(value), valueLength);
    if (field == ":status") {
      std::from_chars(text.data(), text.data() + text.size(), state->head.status);
    } else if (!field.starts_with(':')) {
      state->head.headers.emplace_back(std::string(field), std::string(text));
    }
    return 0;
  }

  static int OnFrame(nghttp2_session* session, const nghttp2_frame* frame, void*) {
    StreamState* state = StateOf(session, frame->hd.stream_id);
    if (!state) return 0;
    if (frame->hd.type == NGHTTP2_HEADERS && !state->headDelivered) {
      if (state->head.status < 200) {
        // An interim response (a 103, say): the real one follows, and this one is not shown.
        state->head = {};
        state->headerBytes = 0;
      } else {
        state->headDelivered = true;
        if (state->sink) state->sink->OnHead(state->head);
      }
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
      state->ended = true;
    }
    return 0;
  }

  static int OnData(nghttp2_session* session, uint8_t, int32_t id, const uint8_t* data, size_t length, void*) {
    StreamState* state = StateOf(session, id);
    if (state && state->sink) state->sink->OnBody({data, length});
    return 0;
  }

  static int OnClose(nghttp2_session*, int32_t id, uint32_t code, void* user) {
    auto* self = static_cast<Http2Session*>(user);
    auto it = self->streams_.find(id);
    if (it == self->streams_.end()) return 0;
    std::unique_ptr<StreamState> state = std::move(it->second);
    self->streams_.erase(it);
    if (!state->sink) return 0;
    if (code == NGHTTP2_NO_ERROR && state->ended) {
      state->sink->OnEnd();
    } else if (!state->failure.empty()) {
      state->sink->OnReset(state->failure, false);
    } else {
      // REFUSED_STREAM promises the request was not processed, so it may be sent again.
      const bool refused = code == NGHTTP2_REFUSED_STREAM && !state->headDelivered;
      state->sink->OnReset(code == NGHTTP2_NO_ERROR ? "the server ended the stream before the response was complete"
                                                    : std::string("the server reset the stream: ") + nghttp2_http2_strerror(code),
                           refused);
    }
    return 0;
  }
};

Http2Session::Http2Session(Transport& transport) : transport_(transport) {}

Http2Session::~Http2Session() {
  if (session_) nghttp2_session_del(session_);
}

bool Http2Session::Start() {
  nghttp2_session_callbacks* callbacks = nullptr;
  if (nghttp2_session_callbacks_new(&callbacks) != 0) return false;
  nghttp2_session_callbacks_set_on_header_callback(callbacks, Callbacks::OnHeader);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, Callbacks::OnFrame);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, Callbacks::OnData);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, Callbacks::OnClose);
  const int made = nghttp2_session_client_new(&session_, callbacks, this);
  nghttp2_session_callbacks_del(callbacks);
  if (made != 0) {
    session_ = nullptr;
    return false;
  }

  const nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
      {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, static_cast<uint32_t>(kMaxHeaderListBytes)},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, static_cast<uint32_t>(kStreamWindow)},
  };
  if (nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, std::size(settings)) != 0) return false;
  if (nghttp2_session_set_local_window_size(session_, NGHTTP2_FLAG_NONE, 0, kConnectionWindow) != 0) return false;
  Flush();
  return true;
}

std::optional<std::string> Http2Session::Receive(std::span<const uint8_t> data) {
  std::optional<std::string> error;
  receiving_ = true;
  while (!data.empty()) {
    const nghttp2_ssize used = nghttp2_session_mem_recv2(session_, data.data(), data.size());
    if (used < 0) {
      error = std::string("HTTP/2 error: ") + nghttp2_strerror(static_cast<int>(used));
      break;
    }
    data = data.subspan(static_cast<size_t>(used));
  }
  receiving_ = false;
  Flush();
  return error;
}

int32_t Http2Session::Submit(const Http2Request& request, Http2Stream& stream) {
  std::vector<nghttp2_nv> fields;
  fields.reserve(request.headers.size() + 4);
  static const std::string kMethod = ":method", kGet = "GET", kScheme = ":scheme", kAuthority = ":authority", kPath = ":path";
  fields.push_back(Field(kMethod, kGet));
  fields.push_back(Field(kScheme, request.scheme));
  fields.push_back(Field(kAuthority, request.authority));
  fields.push_back(Field(kPath, request.path));
  for (const auto& [name, value] : request.headers) fields.push_back(Field(name, value));

  auto state = std::make_unique<StreamState>();
  state->sink = &stream;
  StreamState* raw = state.get();
  // No body: the request ends with its headers.
  const int32_t id = nghttp2_submit_request2(session_, nullptr, fields.data(), fields.size(), nullptr, raw);
  if (id < 0) return id;
  streams_.emplace(id, std::move(state));
  if (!receiving_) Flush();
  return id;
}

void Http2Session::Reset(int32_t id) {
  auto it = streams_.find(id);
  if (it == streams_.end()) return;
  it->second->sink = nullptr;
  nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
  if (!receiving_) Flush();
}

void Http2Session::Shutdown() {
  nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
  if (!receiving_) Flush();
}

bool Http2Session::Available() const {
  if (!session_ || nghttp2_session_check_request_allowed(session_) == 0) return false;
  return streams_.size() < nghttp2_session_get_remote_settings(session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
}

bool Http2Session::Finished() const {
  return !session_ || (nghttp2_session_want_read(session_) == 0 && nghttp2_session_want_write(session_) == 0);
}

// Takes what nghttp2 has to send, which may call back with a stream's end, so it is not re-entered
// from there: the loop here picks up whatever such a call queues.
void Http2Session::Flush() {
  if (flushing_ || !session_) return;
  flushing_ = true;
  std::string out;
  while (true) {
    const uint8_t* bytes = nullptr;
    const nghttp2_ssize length = nghttp2_session_mem_send2(session_, &bytes);
    if (length <= 0) break;
    out.append(reinterpret_cast<const char*>(bytes), static_cast<size_t>(length));
  }
  flushing_ = false;
  if (!out.empty()) transport_.Send(std::move(out));
}

}  // namespace solar::net::internal
