/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>

#include "MseInternal.h"

namespace solar::media {

namespace {

constexpr size_t kQuota = size_t{256} << 20;  // what the buffers of a MediaSource keep, at most

int64_t Micro(double seconds) { return static_cast<int64_t>(std::llround(seconds * 1e6)); }

// An init segment begins with an ftyp box (MP4), or is a WebM's EBML header.
bool LooksLikeInit(const uint8_t* data, size_t size) {
  if (size >= 8 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') return true;
  if (size >= 8 && data[4] == 'm' && data[5] == 'o' && data[6] == 'o' && data[7] == 'v') return true;
  if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) return true;
  return false;
}

void FreePacket(AVPacket* packet) { g_api.av_packet_free(&packet); }

// What an init segment gets after it so that the demuxer does not wait for the first fragment to say it has read the header: FFmpeg's MP4 demuxer reads on until an mdat
// has begun, and the Matroska one until a cluster has. An empty one is such a start; the real ones follow it.
std::string SyntheticTail(const uint8_t* data, size_t size) {
  if (size >= 8 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
    // MP4: the top-level boxes are walked; with a moof or an mdat in them the header is not waiting.
    bool moov = false;
    for (size_t at = 0; at + 8 <= size;) {
      const uint32_t length = (uint32_t{data[at]} << 24) | (uint32_t{data[at + 1]} << 16) | (uint32_t{data[at + 2]} << 8) | data[at + 3];
      const char* type = reinterpret_cast<const char*>(data + at + 4);
      if (!std::strncmp(type, "moov", 4)) moov = true;
      if (!std::strncmp(type, "moof", 4) || !std::strncmp(type, "mdat", 4)) return "";
      if (length < 8) break;
      at += length;
    }
    if (!moov) return "";
    // (A box that is skipped, so that there are the few thousand bytes FFmpeg reads to see what the format is; then an mdat with something in it.)
    std::string tail("\x00\x00\x20\x00" "free", 8);
    tail.append(8184, '\0');
    tail.append("\x00\x00\x00\x09mdat\x00", 9);
    return tail;
  }
  if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
    for (size_t i = 0; i + 4 <= size; ++i) {
      if (data[i] == 0x1F && data[i + 1] == 0x43 && data[i + 2] == 0xB6 && data[i + 3] == 0x75) return "";
    }
    std::string tail("\xEC\x60\x00", 3);  // (an EBML Void of 8192 bytes, as for the MP4)
    tail.append(8192, '\0');
    tail.append("\x1F\x43\xB6\x75\x80", 5);
    return tail;
  }
  return "";
}

}  // namespace

MseCodec::~MseCodec() {
  if (vpar) g_api.avcodec_parameters_free(&vpar);
  if (apar) g_api.avcodec_parameters_free(&apar);
}

// ---- MseSource ----

struct MseSource::Impl {
  std::mutex m;
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  double duration = std::nan("");
  bool ended = false;
  std::atomic<uint64_t> version{0};
  std::function<void()> changed;
  std::function<void()> wake;
  void Changed() {
    ++version;
    std::function<void()> w;
    {
      std::lock_guard lock(m);
      w = wake;
    }
    if (w) w();
  }
};

// ---- MseBuffer ----

struct MseBuffer::Impl {
  MseSource::Impl* source = nullptr;
  std::string mime;
  mutable std::mutex m;
  std::map<int64_t, MsePacket> store[2];
  size_t totalBytes = 0;
  double offset = 0;
  bool updating = false, failed = false;
  bool hasKind[2] = {false, false};
  std::shared_ptr<MseCodec> codec;  // the latest init segment's
  std::string initBytes;
  bool needInit = false;

  struct Session {
    MseBuffer::Impl* owner = nullptr;
    std::shared_ptr<ByteStream> in = std::make_shared<ByteStream>();
    std::thread thread;
    std::atomic<bool> stop{false};
    uint64_t pos = 0;
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;
    int vi = -1, ai = -1;
    bool hadData = false;

    static int ReadCb(void* opaque, uint8_t* buffer, int size) {
      auto* self = static_cast<Session*>(opaque);
      auto stopped = [self] { return self->stop.load(); };
      for (;;) {
        // (First without waiting: when there is nothing, everything that was appended has been taken.)
        int64_t n = self->in->Read(self->pos, buffer, static_cast<size_t>(size), stopped, 0);
        if (n > 0) {
          self->pos += static_cast<uint64_t>(n);
          self->hadData = true;
          return static_cast<int>(n);
        }
        if (n == -1) return AVERROR_EXIT;
        if (n == 0 || n == -3) return AVERROR_EOF;
        self->owner->Idle(self);
        n = self->in->Read(self->pos, buffer, static_cast<size_t>(size), stopped, 200);
        if (n > 0) {
          self->pos += static_cast<uint64_t>(n);
          self->hadData = true;
          return static_cast<int>(n);
        }
        if (n == -1) return AVERROR_EXIT;
        if (n == 0 || n == -3) return AVERROR_EOF;
      }
    }
    static int InterruptCb(void* opaque) { return static_cast<Session*>(opaque)->stop.load() ? 1 : 0; }

    void Run() {
      if (!LoadFfmpeg()) {
        owner->Fail();
        return;
      }
      fmt = g_api.avformat_alloc_context();
      constexpr int kBuffer = 1 << 16;
      uint8_t* buffer = static_cast<uint8_t*>(g_api.av_malloc(kBuffer));
      io = g_api.avio_alloc_context(buffer, kBuffer, 0, this, &ReadCb, nullptr, nullptr);
      fmt->pb = io;
      fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
      fmt->interrupt_callback.callback = &InterruptCb;
      fmt->interrupt_callback.opaque = this;
      if (g_api.avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
        if (!stop) owner->Fail();
        Close();
        return;
      }
      // (The init segment says what the tracks are: there is no waiting for packets to find out.)
      auto codec = std::make_shared<MseCodec>();
      for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vi < 0 && stream->codecpar->width > 0) {
          vi = static_cast<int>(i);
          codec->vpar = g_api.avcodec_parameters_alloc();
          g_api.avcodec_parameters_copy(codec->vpar, stream->codecpar);
          codec->vtb = stream->time_base;
          const AVRational ratio = stream->sample_aspect_ratio.num > 0 ? stream->sample_aspect_ratio : stream->codecpar->sample_aspect_ratio;
          codec->sar = ratio.num > 0 && ratio.den > 0 ? static_cast<double>(ratio.num) / ratio.den : 1;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ai < 0) {
          ai = static_cast<int>(i);
          codec->apar = g_api.avcodec_parameters_alloc();
          g_api.avcodec_parameters_copy(codec->apar, stream->codecpar);
          codec->atb = stream->time_base;
        } else {
          stream->discard = AVDISCARD_ALL;
        }
      }
      if (vi < 0 && ai < 0) {
        owner->Fail();
        Close();
        return;
      }
      owner->OnCodec(codec, vi >= 0, ai >= 0);
      AVPacket* packet = g_api.av_packet_alloc();
      while (!stop) {
        const int r = g_api.av_read_frame(fmt, packet);
        if (r < 0) {
          if (!stop && r != AVERROR_EOF && r != AVERROR_EXIT) owner->Fail();
          break;
        }
        const int kind = packet->stream_index == vi ? 0 : (packet->stream_index == ai ? 1 : -1);
        if (kind >= 0) owner->Store(*this, packet, kind, kind == 0 ? codec->vtb : codec->atb, codec);
        g_api.av_packet_unref(packet);
      }
      g_api.av_packet_free(&packet);
      Close();
    }
    void Close() {
      if (fmt) g_api.avformat_close_input(&fmt);
      if (io) {
        g_api.av_freep(&io->buffer);
        g_api.avio_context_free(&io);
      }
    }
    void Stop() {
      stop = true;
      in->Finish(false);
      in->Poke();
      if (thread.joinable()) thread.join();
    }
  };
  std::shared_ptr<Session> session;

  void Notify() {
    std::function<void()> changed = source->changed;
    if (changed) changed();
  }

  // The parser has taken all that was appended.
  void Idle(Session* from) {
    bool tell = false;
    {
      std::lock_guard lock(m);
      if (session.get() == from && from->pos >= from->in->available() && updating) {
        updating = false;
        tell = true;
      }
    }
    if (tell) Notify();
  }
  void Fail() {
    {
      std::lock_guard lock(m);
      failed = true;
      updating = false;
    }
    Notify();
  }
  void OnCodec(std::shared_ptr<MseCodec> c, bool video, bool audio) {
    {
      std::lock_guard lock(m);
      codec = std::move(c);
      if (video) hasKind[0] = true;
      if (audio) hasKind[1] = true;
    }
    source->Changed();
  }

  // A packet from the demuxer, kept by its time with the offset in it.
  void Store(Session&, AVPacket* in, int kind, AVRational tb, const std::shared_ptr<MseCodec>& c) {
    std::shared_ptr<AVPacket> packet(g_api.av_packet_clone(in), FreePacket);
    if (!packet) return;
    {
      std::lock_guard lock(m);
      const double scale = av_q2d(tb);
      if (packet->dts == AV_NOPTS_VALUE) packet->dts = packet->pts;
      if (packet->pts == AV_NOPTS_VALUE) packet->pts = packet->dts;
      if (packet->dts == AV_NOPTS_VALUE) return;
      if (offset != 0) {
        const int64_t ticks = static_cast<int64_t>(std::llround(offset / scale));
        packet->pts += ticks;
        packet->dts += ticks;
      }
      MsePacket kept;
      kept.dts = static_cast<double>(packet->dts) * scale;
      kept.pts = static_cast<double>(packet->pts) * scale;
      kept.duration = static_cast<double>(packet->duration) * scale;
      kept.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
      kept.bytes = static_cast<size_t>(packet->size);
      kept.codec = c;
      kept.packet = std::move(packet);
      int64_t key = Micro(kept.dts);
      auto& map = store[kind];
      const auto existing = map.find(key);
      if (existing != map.end()) {
        totalBytes -= existing->second.bytes;
        map.erase(existing);
      }
      totalBytes += kept.bytes;
      map.emplace(key, std::move(kept));
    }
    source->Changed();
  }

  static std::vector<TimeRange> RangesOf(const std::map<int64_t, MsePacket>& map) {
    std::vector<TimeRange> ranges;
    for (const auto& [key, packet] : map) {
      const double start = packet.dts, end = packet.dts + std::max(packet.duration, 0.0);
      if (!ranges.empty() && start <= ranges.back().end + 0.1) ranges.back().end = std::max(ranges.back().end, end);
      else ranges.push_back({start, end});
    }
    return ranges;
  }
};

MseBuffer::MseBuffer() : impl_(std::make_unique<Impl>()) {}

MseBuffer::~MseBuffer() {
  std::shared_ptr<Impl::Session> session;
  {
    std::lock_guard lock(impl_->m);
    session = std::move(impl_->session);
  }
  if (session) session->Stop();
}

bool MseBuffer::Append(const uint8_t* data, size_t size) {
  std::shared_ptr<Impl::Session> old;
  std::shared_ptr<Impl::Session> use;
  std::string feed;
  {
    std::lock_guard lock(impl_->m);
    if (impl_->totalBytes + size > kQuota) return false;
    const bool init = LooksLikeInit(data, size);
    if (init || !impl_->session) {
      if (impl_->session && (impl_->session->hadData || init)) old = std::move(impl_->session);
      else if (impl_->session) old = std::move(impl_->session);
      impl_->session = std::make_shared<Impl::Session>();
      impl_->session->owner = impl_.get();
      Impl::Session* raw = impl_->session.get();
      impl_->session->thread = std::thread([raw] { raw->Run(); });
      if (init) {
        impl_->initBytes.assign(reinterpret_cast<const char*>(data), size);
        if (impl_->initBytes.size() > (size_t{4} << 20)) impl_->initBytes.clear();
      } else if (!impl_->initBytes.empty()) {
        // (After an abort the parser starts again: it is given the init segment first.)
        feed = impl_->initBytes;
      }
    }
    use = impl_->session;
    impl_->updating = true;
    impl_->failed = false;
  }
  if (old) old->Stop();
  if (!feed.empty()) use->in->Append(reinterpret_cast<const uint8_t*>(feed.data()), feed.size());
  use->in->Append(data, size);
  if (LooksLikeInit(data, size)) {
    const std::string tail = SyntheticTail(data, size);
    if (!tail.empty()) use->in->Append(reinterpret_cast<const uint8_t*>(tail.data()), tail.size());
  }
  return true;
}

bool MseBuffer::updating() const { std::lock_guard lock(impl_->m); return impl_->updating; }
bool MseBuffer::failed() const { std::lock_guard lock(impl_->m); return impl_->failed; }
size_t MseBuffer::bytes() const { std::lock_guard lock(impl_->m); return impl_->totalBytes; }

void MseBuffer::Abort() {
  std::shared_ptr<Impl::Session> old;
  bool tell = false;
  {
    std::lock_guard lock(impl_->m);
    if (!impl_->updating) return;
    old = std::move(impl_->session);
    impl_->updating = false;
    tell = true;
  }
  if (old) old->Stop();
  if (tell) impl_->Notify();
}

void MseBuffer::Remove(double start, double end) {
  {
    std::lock_guard lock(impl_->m);
    for (int kind = 0; kind < 2; ++kind) {
      auto& map = impl_->store[kind];
      auto it = map.lower_bound(Micro(start));
      const auto last = map.lower_bound(Micro(end));
      while (it != last) {
        impl_->totalBytes -= it->second.bytes;
        it = map.erase(it);
      }
      // (Pictures that follow and need what was removed go with it, up to the next one that stands alone.)
      if (kind == 0) {
        while (it != map.end() && !it->second.key) {
          impl_->totalBytes -= it->second.bytes;
          it = map.erase(it);
        }
      }
    }
  }
  impl_->source->Changed();
}

void MseBuffer::SetTimestampOffset(double seconds) {
  std::lock_guard lock(impl_->m);
  impl_->offset = seconds;
}

std::vector<TimeRange> MseBuffer::Buffered(int kind) {
  std::lock_guard lock(impl_->m);
  if (kind >= 0) return Impl::RangesOf(impl_->store[kind]);
  const bool v = !impl_->store[0].empty(), a = !impl_->store[1].empty();
  if (v && !a) return Impl::RangesOf(impl_->store[0]);
  if (a && !v) return Impl::RangesOf(impl_->store[1]);
  if (!v && !a) return {};
  // (Where both have something.)
  const auto vr = Impl::RangesOf(impl_->store[0]), ar = Impl::RangesOf(impl_->store[1]);
  std::vector<TimeRange> out;
  for (const TimeRange& x : vr) {
    for (const TimeRange& y : ar) {
      const double s = std::max(x.start, y.start), e = std::min(x.end, y.end);
      if (e > s) out.push_back({s, e});
    }
  }
  return out;
}

bool MseBuffer::HasTrack(int kind) {
  std::lock_guard lock(impl_->m);
  return impl_->hasKind[kind];
}

// ---- MseSource ----

MseSource::MseSource() : impl_(std::make_unique<Impl>()) {}
MseSource::~MseSource() = default;

std::shared_ptr<MseSource> MseSource::Create(std::function<void()> changed) {
  std::shared_ptr<MseSource> source(new MseSource());
  source->impl_->changed = std::move(changed);
  return source;
}

std::shared_ptr<MseBuffer> MseSource::AddBuffer(const std::string& mime) {
  const size_t semicolon = mime.find(';');
  std::string type = semicolon == std::string::npos ? mime : mime.substr(0, semicolon);
  std::string codecs;
  if (semicolon != std::string::npos) {
    const size_t at = mime.find("codecs", semicolon);
    if (at != std::string::npos) {
      const size_t open = mime.find_first_of("\"'", at);
      const size_t close = open == std::string::npos ? std::string::npos : mime.find(mime[open], open + 1);
      if (open != std::string::npos && close != std::string::npos) codecs = mime.substr(open + 1, close - open - 1);
    }
  }
  if (CanPlayType(type, codecs) == 0) return nullptr;
  std::shared_ptr<MseBuffer> buffer(new MseBuffer());
  buffer->impl_->source = impl_.get();
  buffer->impl_->mime = mime;
  std::lock_guard lock(impl_->m);
  impl_->buffers.push_back(buffer);
  return buffer;
}

void MseSource::RemoveBuffer(const std::shared_ptr<MseBuffer>& buffer) {
  {
    std::lock_guard lock(impl_->m);
    impl_->buffers.erase(std::remove(impl_->buffers.begin(), impl_->buffers.end(), buffer), impl_->buffers.end());
  }
  impl_->Changed();
}

void MseSource::SetDuration(double seconds) {
  {
    std::lock_guard lock(impl_->m);
    impl_->duration = seconds;
  }
  impl_->Changed();
}
double MseSource::duration() const { std::lock_guard lock(impl_->m); return impl_->duration; }
void MseSource::EndOfStream() {
  {
    std::lock_guard lock(impl_->m);
    impl_->ended = true;
  }
  impl_->Changed();
}
void MseSource::Reopen() {
  std::lock_guard lock(impl_->m);
  impl_->ended = false;
}
bool MseSource::ended() const { std::lock_guard lock(impl_->m); return impl_->ended; }
uint64_t MseSource::version() const { return impl_->version.load(); }
void MseSource::SetWake(std::function<void()> wake) {
  std::lock_guard lock(impl_->m);
  impl_->wake = std::move(wake);
}

std::vector<TimeRange> MseSource::Buffered() {
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  {
    std::lock_guard lock(impl_->m);
    buffers = impl_->buffers;
  }
  std::vector<TimeRange> kinds[2];
  bool any[2] = {false, false};
  for (const auto& buffer : buffers) {
    for (int kind = 0; kind < 2; ++kind) {
      if (!buffer->HasTrack(kind)) continue;
      any[kind] = true;
      for (const TimeRange& r : buffer->Buffered(kind)) kinds[kind].push_back(r);
    }
  }
  auto merge = [](std::vector<TimeRange> v) {
    std::sort(v.begin(), v.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> out;
    for (const TimeRange& r : v) {
      if (!out.empty() && r.start <= out.back().end + 0.1) out.back().end = std::max(out.back().end, r.end);
      else out.push_back(r);
    }
    return out;
  };
  if (any[0] && !any[1]) return merge(kinds[0]);
  if (any[1] && !any[0]) return merge(kinds[1]);
  if (!any[0] && !any[1]) return {};
  const auto vr = merge(kinds[0]), ar = merge(kinds[1]);
  std::vector<TimeRange> out;
  for (const TimeRange& x : vr) {
    for (const TimeRange& y : ar) {
      const double s = std::max(x.start, y.start), e = std::min(x.end, y.end);
      if (e > s) out.push_back({s, e});
    }
  }
  return out;
}

bool MseSource::HasTrack(int kind) {
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  {
    std::lock_guard lock(impl_->m);
    buffers = impl_->buffers;
  }
  for (const auto& b : buffers) if (b->HasTrack(kind)) return true;
  return false;
}

std::shared_ptr<MseCodec> MseSource::CodecOf(int kind) {
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  {
    std::lock_guard lock(impl_->m);
    buffers = impl_->buffers;
  }
  for (const auto& b : buffers) {
    std::lock_guard lock(b->impl_->m);
    if (b->impl_->hasKind[kind] && b->impl_->codec) return b->impl_->codec;
  }
  return nullptr;
}

bool MseSource::Peek(int kind, double after, MsePacket& out, bool& gap) {
  gap = false;
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  {
    std::lock_guard lock(impl_->m);
    buffers = impl_->buffers;
  }
  bool found = false;
  for (const auto& b : buffers) {
    std::lock_guard lock(b->impl_->m);
    auto& map = b->impl_->store[kind];
    const auto it = map.upper_bound(Micro(after));
    if (it == map.end()) continue;
    if (!found || it->second.dts < out.dts) {
      out = it->second;
      found = true;
    }
  }
  if (found && after > -1e8 && out.dts - after > 1.0) {
    gap = true;
    return false;
  }
  return found;
}

bool MseSource::FindStart(int kind, double target, double& dts) {
  std::vector<std::shared_ptr<MseBuffer>> buffers;
  {
    std::lock_guard lock(impl_->m);
    buffers = impl_->buffers;
  }
  for (const auto& b : buffers) {
    if (!b->HasTrack(kind)) continue;
    std::lock_guard lock(b->impl_->m);
    auto& map = b->impl_->store[kind];
    auto it = map.upper_bound(Micro(target));
    // The packet to begin at: the last one at or before the target (for pictures, one that stands alone).
    bool ok = false;
    while (it != map.begin()) {
      --it;
      if (kind == 1 || it->second.key) { ok = true; break; }
    }
    if (!ok) continue;
    const double start = it->second.dts;
    // The target is in the same stretch of what is buffered as the start is.
    double end = start;
    for (auto walk = it; walk != map.end(); ++walk) {
      if (walk->second.dts > end + 0.1 + std::max(walk->second.duration, 0.0) && walk->second.dts > end + 0.5) break;
      end = std::max(end, walk->second.dts + std::max(walk->second.duration, 0.0));
      if (end >= target) break;
    }
    if (end + 0.05 < target) continue;
    dts = start;
    return true;
  }
  return false;
}

}  // namespace solar::media
