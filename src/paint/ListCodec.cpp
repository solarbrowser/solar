/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/paint/ListCodec.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include "CanvasSurface.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "solar/font/Face.h"
#include "solar/media/Player.h"

namespace solar::paint {

namespace {

class Writer {
 public:
  void U8(uint8_t v) { bytes_.push_back(static_cast<char>(v)); }
  void Bool(bool v) { U8(v ? 1 : 0); }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void F32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    U32(bits);
  }
  void F64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    U64(bits);
  }
  void String(const std::string& s) { Bytes(s.data(), s.size()); }
  void Bytes(const void* data, size_t size) {
    U32(static_cast<uint32_t>(size));
    bytes_.append(static_cast<const char*>(data), size);
  }
  std::string Take() { return std::move(bytes_); }
  size_t size() const { return bytes_.size(); }

 private:
  std::string bytes_;
};

class Reader {
 public:
  explicit Reader(const std::string& bytes) : bytes_(bytes) {}
  bool ok() const { return ok_; }
  size_t Left() const { return bytes_.size() - pos_; }
  uint8_t U8() {
    if (Left() < 1) { ok_ = false; return 0; }
    return static_cast<uint8_t>(bytes_[pos_++]);
  }
  bool Bool() { return U8() != 0; }
  uint32_t U32() {
    if (Left() < 4) { ok_ = false; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
    pos_ += 4;
    return v;
  }
  uint64_t U64() {
    if (Left() < 8) { ok_ = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
    pos_ += 8;
    return v;
  }
  float F32() {
    const uint32_t bits = U32();
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
  }
  double F64() {
    const uint64_t bits = U64();
    double v;
    std::memcpy(&v, &bits, 8);
    return std::isfinite(v) ? v : 0.0;
  }
  std::string String(size_t limit = size_t{1} << 28) {
    const uint32_t size = U32();
    if (!ok_ || size > Left() || size > limit) { ok_ = false; return {}; }
    std::string out = bytes_.substr(pos_, size);
    pos_ += size;
    return out;
  }
  // A count of things that each take at least `each` bytes: one that could not be there is not one.
  uint32_t Count(size_t each) {
    const uint32_t n = U32();
    if (!ok_ || (each > 0 && n > Left() / each)) { ok_ = false; return 0; }
    return n;
  }
  template <typename Enum>
  Enum EnumUpTo(Enum last) {
    const uint32_t v = U8();
    if (v > static_cast<uint32_t>(last)) { ok_ = false; return static_cast<Enum>(0); }
    return static_cast<Enum>(v);
  }

 private:
  const std::string& bytes_;
  size_t pos_ = 0;
  bool ok_ = true;
};

constexpr int kMostPixels = 16384;

// ---- The parts of a list ----

void WriteColor(Writer& w, const Color& c) { w.U8(c.r); w.U8(c.g); w.U8(c.b); w.U8(c.a); }
Color ReadColor(Reader& r) {
  Color c;
  c.r = r.U8(); c.g = r.U8(); c.b = r.U8(); c.a = r.U8();
  return c;
}
void WriteRect(Writer& w, const RectF& v) { w.F32(v.x); w.F32(v.y); w.F32(v.width); w.F32(v.height); }
RectF ReadRect(Reader& r) {
  RectF v;
  v.x = r.F32(); v.y = r.F32(); v.width = r.F32(); v.height = r.F32();
  return v;
}
void WriteRounded(Writer& w, const RoundedRect& v) {
  WriteRect(w, v.rect);
  for (float f : v.radii) w.F32(f);
}
RoundedRect ReadRounded(Reader& r) {
  RoundedRect v;
  v.rect = ReadRect(r);
  for (float& f : v.radii) f = r.F32();
  return v;
}
void WriteLength(Writer& w, const Length& v) { w.F32(v.px); w.F32(v.percent); w.Bool(v.automatic); }
Length ReadLength(Reader& r) {
  Length v;
  v.px = r.F32(); v.percent = r.F32(); v.automatic = r.Bool();
  return v;
}
void WriteShadow(Writer& w, const Shadow& s) {
  WriteColor(w, s.color);
  w.F32(s.dx); w.F32(s.dy); w.F32(s.blur); w.F32(s.spread);
  w.Bool(s.inset);
}
Shadow ReadShadow(Reader& r) {
  Shadow s;
  s.color = ReadColor(r);
  s.dx = r.F32(); s.dy = r.F32(); s.blur = r.F32(); s.spread = r.F32();
  s.inset = r.Bool();
  return s;
}
void WriteGradient(Writer& w, const Gradient& g) {
  w.U8(static_cast<uint8_t>(g.kind));
  w.Bool(g.repeating);
  w.F32(g.angle);
  w.Bool(g.toCorner);
  w.U32(static_cast<uint32_t>(g.cornerX));
  w.U32(static_cast<uint32_t>(g.cornerY));
  w.Bool(g.circle);
  w.U8(static_cast<uint8_t>(g.extent));
  WriteLength(w, g.radiusX); WriteLength(w, g.radiusY);
  WriteLength(w, g.centerX); WriteLength(w, g.centerY);
  w.U32(static_cast<uint32_t>(g.stops.size()));
  for (const ColorStop& s : g.stops) {
    WriteColor(w, s.color);
    WriteLength(w, s.position);
    w.Bool(s.positioned);
  }
}
std::shared_ptr<const Gradient> ReadGradient(Reader& r) {
  auto g = std::make_shared<Gradient>();
  g->kind = r.EnumUpTo(Gradient::Kind::Conic);
  g->repeating = r.Bool();
  g->angle = r.F32();
  g->toCorner = r.Bool();
  g->cornerX = static_cast<int>(r.U32());
  g->cornerY = static_cast<int>(r.U32());
  g->circle = r.Bool();
  g->extent = r.EnumUpTo(Gradient::Extent::Lengths);
  g->radiusX = ReadLength(r); g->radiusY = ReadLength(r);
  g->centerX = ReadLength(r); g->centerY = ReadLength(r);
  const uint32_t n = r.Count(15);
  if (n > 4096) return nullptr;
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    ColorStop s;
    s.color = ReadColor(r);
    s.position = ReadLength(r);
    s.positioned = r.Bool();
    g->stops.push_back(s);
  }
  return g;
}
void WriteSticky(Writer& w, const StickyInfo& s) {
  w.Bool(s.valid);
  for (double v : {s.portWidth, s.portHeight, s.x, s.y, s.width, s.height, s.marginTop, s.marginRight, s.marginBottom, s.marginLeft, s.containerX, s.containerY, s.containerWidth, s.containerHeight}) w.F64(v);
  w.Bool(s.hasTop); w.Bool(s.hasRight); w.Bool(s.hasBottom); w.Bool(s.hasLeft);
  for (double v : {s.top, s.right, s.bottom, s.left}) w.F64(v);
  w.Bool(s.rtl);
}
StickyInfo ReadSticky(Reader& r) {
  StickyInfo s;
  s.valid = r.Bool();
  for (double* v : {&s.portWidth, &s.portHeight, &s.x, &s.y, &s.width, &s.height, &s.marginTop, &s.marginRight, &s.marginBottom, &s.marginLeft, &s.containerX, &s.containerY, &s.containerWidth, &s.containerHeight}) *v = r.F64();
  s.hasTop = r.Bool(); s.hasRight = r.Bool(); s.hasBottom = r.Bool(); s.hasLeft = r.Bool();
  for (double* v : {&s.top, &s.right, &s.bottom, &s.left}) *v = r.F64();
  s.rtl = r.Bool();
  return s;
}

void WriteState(Writer& w, const media::PlayerState& s) {
  w.U32(static_cast<uint32_t>(s.readyState));
  w.F64(s.duration);
  w.U32(static_cast<uint32_t>(s.videoWidth)); w.U32(static_cast<uint32_t>(s.videoHeight));
  w.Bool(s.hasVideo); w.Bool(s.hasAudio);
  w.F64(s.currentTime);
  w.Bool(s.seeking); w.Bool(s.ended); w.Bool(s.stalled);
  w.U32(static_cast<uint32_t>(s.error));
  w.U32(static_cast<uint32_t>(s.buffered.size()));
  for (const media::TimeRange& t : s.buffered) { w.F64(t.start); w.F64(t.end); }
  w.Bool(s.playing);
  w.F64(s.volume);
  w.Bool(s.muted); w.Bool(s.controlsVisible); w.Bool(s.fullscreen);
}
media::PlayerState ReadState(Reader& r) {
  media::PlayerState s;
  s.readyState = static_cast<int>(r.U32());
  s.duration = r.F64();
  s.videoWidth = static_cast<int>(r.U32()); s.videoHeight = static_cast<int>(r.U32());
  s.hasVideo = r.Bool(); s.hasAudio = r.Bool();
  s.currentTime = r.F64();
  s.seeking = r.Bool(); s.ended = r.Bool(); s.stalled = r.Bool();
  s.error = static_cast<int>(r.U32());
  const uint32_t n = r.Count(16);
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    media::TimeRange t;
    t.start = r.F64(); t.end = r.F64();
    s.buffered.push_back(t);
  }
  s.playing = r.Bool();
  s.volume = r.F64();
  s.muted = r.Bool(); s.controlsVisible = r.Bool(); s.fullscreen = r.Bool();
  return s;
}

// What stands for a video where its pictures are drawn: the last frame and state it was given.
class RemoteVideo final : public media::VideoSource {
 public:
  media::PlayerState State() override { return state; }
  std::shared_ptr<const media::VideoFrame> Frame() override { return frame; }
  media::PlayerState state;
  std::shared_ptr<const media::VideoFrame> frame;
};

class RemoteCanvas final : public CanvasSurface {
 public:
  RemoteCanvas() : CanvasSurface(1, 1) {}
  sk_sp<SkImage> Latest() const override { return image; }
  sk_sp<SkImage> image;
};

}  // namespace

// ---- Encoder ----

uint64_t ListEncoder::IdOf(const void* key, const std::shared_ptr<const void>& owner, bool& isNew) {
  Known& known = known_[key];
  if (known.id != 0 && known.alive.lock() == owner) {
    isNew = false;
    return known.id;
  }
  // (An address a thing that was let go of had is no longer that thing: what comes there is a new one.)
  known.id = next_++;
  known.alive = owner;
  isNew = true;
  return known.id;
}

std::vector<EncodedMessage> ListEncoder::Encode(const DisplayList& list) {
  std::vector<EncodedMessage> messages;
  videos_.clear();
  canvases_.clear();
  live_.clear();

  const auto faceId = [&](const std::shared_ptr<font::Face>& face) -> uint64_t {
    if (!face) return 0;
    bool isNew = false;
    const uint64_t id = IdOf(face.get(), face, isNew);
    if (isNew) {
      Writer w;
      w.U64(id);
      const std::string& path = face->SourcePath();
      w.Bool(!path.empty());
      if (!path.empty()) {
        w.String(path);
      } else {
        const std::string_view data = face->FileData();
        if (data.size() > kMostPerMessage) { known_.erase(face.get()); return 0; }
        w.Bytes(data.data(), data.size());
      }
      w.U32(face->Index());
      w.U32(static_cast<uint32_t>(face->Variations().size()));
      for (const font::Face::Axis& axis : face->Variations()) {
        w.String(axis.tag);
        w.F64(axis.value);
      }
      messages.push_back({ListMessage::DefineFace, w.Take()});
    }
    return id;
  };
  const auto imageId = [&](const ImageRef& image) -> uint64_t {
    if (!image || !image->bytes) return 0;
    bool isNew = false;
    const uint64_t id = IdOf(image.get(), image, isNew);
    if (isNew) {
      if (image->bytes->size() > kMostPerMessage) { known_.erase(image.get()); return 0; }
      Writer w;
      w.U64(id);
      w.Bytes(image->bytes->data(), image->bytes->size());
      messages.push_back({ListMessage::DefineImage, w.Take()});
    }
    return id;
  };
  const auto layer = [&](Writer& w, const ImageLayer& l) {
    w.U64(imageId(l.image));
    w.Bool(l.gradient != nullptr);
    if (l.gradient) WriteGradient(w, *l.gradient);
    WriteRounded(w, l.clip);
    WriteRect(w, l.origin);
    w.U8(static_cast<uint8_t>(l.size));
    WriteLength(w, l.width); WriteLength(w, l.height);
    WriteLength(w, l.x); WriteLength(w, l.y);
    w.U8(static_cast<uint8_t>(l.repeatX)); w.U8(static_cast<uint8_t>(l.repeatY));
    w.Bool(l.pixelated);
  };

  Writer w;
  w.F32(list.scrollX); w.F32(list.scrollY); w.F32(list.roomY); w.F32(list.width); w.F32(list.height);
  w.U32(static_cast<uint32_t>(list.items.size()));
  for (const Item& item : list.items) {
    w.U8(static_cast<uint8_t>(item.index()));
    switch (item.index()) {
      case 0: {
        const auto& v = std::get<FillRoundedRect>(item);
        WriteRounded(w, v.rect); WriteColor(w, v.color);
        break;
      }
      case 1: {
        const auto& v = std::get<DrawBorder>(item);
        WriteRounded(w, v.outer); WriteRounded(w, v.inner);
        for (float f : v.widths) w.F32(f);
        for (LineStyle s : v.styles) w.U8(static_cast<uint8_t>(s));
        for (const Color& c : v.colors) WriteColor(w, c);
        break;
      }
      case 2: {
        const auto& v = std::get<DrawGlyphs>(item);
        w.U64(faceId(v.face));
        w.F32(v.size); WriteColor(w, v.color); w.F32(v.x); w.F32(v.y);
        w.U32(static_cast<uint32_t>(v.glyphs.size()));
        for (uint32_t g : v.glyphs) w.U32(g);
        w.U32(static_cast<uint32_t>(v.offsets.size()));
        for (float f : v.offsets) w.F32(f);
        w.U32(static_cast<uint32_t>(v.shadows.size()));
        for (const Shadow& s : v.shadows) WriteShadow(w, s);
        break;
      }
      case 3: WriteRounded(w, std::get<PushClip>(item).rect); break;
      case 4: break;
      case 5: w.F32(std::get<PushLayer>(item).opacity); break;
      case 6: break;
      case 7: {
        const auto& v = std::get<PushTransform>(item);
        for (float f : v.matrix.m) w.F32(f);
        w.Bool(v.followsScroll);
        WriteSticky(w, v.sticky);
        break;
      }
      case 8: break;
      case 9: layer(w, std::get<DrawImageLayer>(item).layer); break;
      case 10: {
        const auto& v = std::get<DrawImage>(item);
        w.U64(imageId(v.image));
        WriteRounded(w, v.clip);
        w.U8(static_cast<uint8_t>(v.fit));
        WriteLength(w, v.x); WriteLength(w, v.y);
        w.Bool(v.pixelated);
        break;
      }
      case 11: {
        const auto& v = std::get<DrawVideo>(item);
        uint64_t id = 0;
        if (v.player) {
          bool isNew = false;
          id = IdOf(v.player.get(), v.player, isNew);
          videos_.push_back({id, v.player, 0, false});
        }
        w.U64(id);
        WriteRounded(w, v.clip);
        w.U8(static_cast<uint8_t>(v.fit));
        WriteLength(w, v.x); WriteLength(w, v.y);
        w.U64(imageId(v.poster));
        w.Bool(v.controls);
        w.String(v.captions);
        break;
      }
      case 12: {
        const auto& v = std::get<DrawCanvas>(item);
        uint64_t id = 0;
        if (v.surface) {
          bool isNew = false;
          id = IdOf(v.surface.get(), v.surface, isNew);
          canvases_.push_back({id, v.surface, 0});
        }
        w.U64(id);
        WriteRounded(w, v.clip);
        w.U8(static_cast<uint8_t>(v.fit));
        WriteLength(w, v.x); WriteLength(w, v.y);
        w.Bool(v.pixelated);
        break;
      }
      case 13: break;
      case 14: {
        const auto& v = std::get<PopMask>(item);
        w.U32(static_cast<uint32_t>(v.layers.size()));
        for (const ImageLayer& l : v.layers) layer(w, l);
        break;
      }
      case 15: {
        const auto& v = std::get<DrawBoxShadow>(item);
        WriteRounded(w, v.shape); WriteShadow(w, v.shadow);
        break;
      }
      case 16: {
        const auto& v = std::get<DrawOutline>(item);
        WriteRounded(w, v.shape);
        w.F32(v.offset); w.F32(v.width);
        w.U8(static_cast<uint8_t>(v.style));
        WriteColor(w, v.color);
        break;
      }
    }
  }
  messages.push_back({ListMessage::SetList, w.Take()});

  // What nothing has any more is let go of on the other side.
  Writer release;
  uint32_t count = 0;
  Writer ids;
  for (auto it = known_.begin(); it != known_.end();) {
    if (it->second.alive.expired()) {
      ids.U64(it->second.id);
      ++count;
      it = known_.erase(it);
    } else {
      ++it;
    }
  }
  if (count) {
    release.U32(count);
    std::string tail = ids.Take();
    std::string bytes = release.Take();
    bytes += tail;
    messages.push_back({ListMessage::Release, std::move(bytes)});
  }
  return messages;
}

std::vector<EncodedMessage> ListEncoder::Dynamic() {
  std::vector<EncodedMessage> messages;
  for (Video& video : videos_) {
    if (std::shared_ptr<const media::VideoFrame> frame = video.source->Frame()) {
      if (frame->width > 0 && frame->height > 0 && frame->width <= kMostPixels && frame->height <= kMostPixels && frame->bgra.size() == static_cast<size_t>(frame->width) * frame->height * 4 &&
          (!video.sentAny || frame->serial != video.sentSerial)) {
        Writer w;
        w.U64(video.id);
        w.U8(0);
        w.U32(static_cast<uint32_t>(frame->width)); w.U32(static_cast<uint32_t>(frame->height));
        w.U64(frame->serial);
        w.Bytes(frame->bgra.data(), frame->bgra.size());
        messages.push_back({ListMessage::DefineBitmap, w.Take()});
        video.sentSerial = frame->serial;
        video.sentAny = true;
      }
    }
    Writer w;
    w.U64(video.id);
    WriteState(w, video.source->State());
    messages.push_back({ListMessage::VideoState, w.Take()});
  }
  for (Canvas& canvas : canvases_) {
    sk_sp<SkImage> image = canvas.surface->Latest();
    if (!image || image->uniqueID() == canvas.sentImage) continue;
    const int width = image->width(), height = image->height();
    if (width <= 0 || height <= 0 || width > kMostPixels || height > kMostPixels) continue;
    std::string pixels(static_cast<size_t>(width) * height * 4, '\0');
    const SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    if (!image->readPixels(nullptr, info, pixels.data(), static_cast<size_t>(width) * 4, 0, 0)) continue;
    Writer w;
    w.U64(canvas.id);
    w.U8(1);
    w.U32(static_cast<uint32_t>(width)); w.U32(static_cast<uint32_t>(height));
    w.U64(0);
    w.Bytes(pixels.data(), pixels.size());
    messages.push_back({ListMessage::DefineBitmap, w.Take()});
    canvas.sentImage = image->uniqueID();
  }
  return messages;
}

// ---- Decoder ----

struct ListDecoder::Impl {
  std::map<uint64_t, std::shared_ptr<font::Face>> faces;
  std::map<uint64_t, ImageRef> images;
  std::map<uint64_t, std::shared_ptr<RemoteVideo>> videos;
  std::map<uint64_t, std::shared_ptr<RemoteCanvas>> canvases;
  std::vector<std::string> fontDirectories;
  std::shared_ptr<const DisplayList> current;
  size_t bytes = 0;  // (what is held, for the bound)

  static constexpr size_t kMostBytes = size_t{1} << 30;

  bool PathAllowed(const std::string& path) const {
    std::error_code error;
    const std::filesystem::path real = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    for (const std::string& directory : fontDirectories) {
      const std::filesystem::path base = std::filesystem::weakly_canonical(directory, error);
      if (error) continue;
      const std::string a = real.string(), b = base.string();
      if (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/') return true;
    }
    return false;
  }
};

ListDecoder::ListDecoder() : impl_(std::make_unique<Impl>()) {}
ListDecoder::~ListDecoder() = default;

std::shared_ptr<const DisplayList> ListDecoder::list() const { return impl_->current; }

void ListDecoder::AllowFontDirectories(std::vector<std::string> directories) { impl_->fontDirectories = std::move(directories); }

bool ListDecoder::Apply(ListMessage type, const std::string& data) {
  Impl& s = *impl_;
  Reader r(data);
  switch (type) {
    case ListMessage::DefineFace: {
      const uint64_t id = r.U64();
      const bool byPath = r.Bool();
      std::string source = r.String(size_t{64} << 20);
      const unsigned index = r.U32();
      const uint32_t axes = r.Count(12);
      std::vector<font::Face::Axis> variations;
      for (uint32_t i = 0; i < axes && r.ok(); ++i) {
        font::Face::Axis axis;
        axis.tag = r.String(4);
        axis.value = r.F64();
        variations.push_back(std::move(axis));
      }
      if (!r.ok() || id == 0 || index > 4096) return false;
      std::shared_ptr<font::Face> face;
      if (byPath) {
        if (!s.PathAllowed(source)) return false;
        face = font::Face::OpenFile(source, index);
      } else {
        s.bytes += source.size();
        face = font::Face::Open(std::move(source), index);
      }
      if (face && !variations.empty()) face = face->WithVariations(variations);
      if (s.bytes > Impl::kMostBytes) return false;
      s.faces[id] = std::move(face);
      return true;
    }
    case ListMessage::DefineImage: {
      const uint64_t id = r.U64();
      std::string bytes = r.String(size_t{64} << 20);
      if (!r.ok() || id == 0) return false;
      s.bytes += bytes.size();
      if (s.bytes > Impl::kMostBytes) return false;
      auto image = std::make_shared<ImageData>();
      image->bytes = std::make_shared<const std::string>(std::move(bytes));
      s.images[id] = std::move(image);
      return true;
    }
    case ListMessage::DefineBitmap: {
      const uint64_t id = r.U64();
      const uint8_t kind = r.U8();
      const uint32_t width = r.U32(), height = r.U32();
      const uint64_t serial = r.U64();
      std::string pixels = r.String(size_t{256} << 20);
      if (!r.ok() || id == 0 || kind > 1 || width == 0 || height == 0 || width > kMostPixels || height > kMostPixels || pixels.size() != static_cast<size_t>(width) * height * 4) return false;
      if (kind == 0) {
        auto frame = std::make_shared<media::VideoFrame>();
        frame->width = static_cast<int>(width);
        frame->height = static_cast<int>(height);
        frame->bgra.assign(pixels.begin(), pixels.end());
        frame->serial = serial;
        auto& video = s.videos[id];
        if (!video) video = std::make_shared<RemoteVideo>();
        video->frame = std::move(frame);
      } else {
        const SkImageInfo info = SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        auto& canvas = s.canvases[id];
        if (!canvas) canvas = std::make_shared<RemoteCanvas>();
        canvas->image = SkImages::RasterFromPixmapCopy(SkPixmap(info, pixels.data(), static_cast<size_t>(width) * 4));
      }
      return true;
    }
    case ListMessage::VideoState: {
      const uint64_t id = r.U64();
      media::PlayerState state = ReadState(r);
      if (!r.ok() || id == 0) return false;
      auto& video = s.videos[id];
      if (!video) video = std::make_shared<RemoteVideo>();
      video->state = std::move(state);
      return true;
    }
    case ListMessage::Release: {
      const uint32_t n = r.Count(8);
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const uint64_t id = r.U64();
        s.faces.erase(id);
        s.images.erase(id);
        s.videos.erase(id);
        s.canvases.erase(id);
      }
      return r.ok();
    }
    case ListMessage::SetList: {
      auto list = std::make_shared<DisplayList>();
      list->scrollX = r.F32(); list->scrollY = r.F32(); list->roomY = r.F32(); list->width = r.F32(); list->height = r.F32();
      const uint32_t count = r.Count(1);
      if (count > 20'000'000) return false;
      list->items.reserve(count);
      const auto image = [&](uint64_t id) -> ImageRef {
        const auto found = s.images.find(id);
        return found == s.images.end() ? nullptr : found->second;
      };
      const auto layer = [&](ImageLayer& l) {
        l.image = image(r.U64());
        if (r.Bool()) l.gradient = ReadGradient(r);
        l.clip = ReadRounded(r);
        l.origin = ReadRect(r);
        l.size = r.EnumUpTo(ImageLayer::Size::Lengths);
        l.width = ReadLength(r); l.height = ReadLength(r);
        l.x = ReadLength(r); l.y = ReadLength(r);
        l.repeatX = r.EnumUpTo(Repeat::Round); l.repeatY = r.EnumUpTo(Repeat::Round);
        l.pixelated = r.Bool();
      };
      for (uint32_t i = 0; i < count && r.ok(); ++i) {
        switch (r.U8()) {
          case 0: { FillRoundedRect v; v.rect = ReadRounded(r); v.color = ReadColor(r); list->items.push_back(v); break; }
          case 1: {
            DrawBorder v;
            v.outer = ReadRounded(r); v.inner = ReadRounded(r);
            for (float& f : v.widths) f = r.F32();
            for (LineStyle& st : v.styles) st = r.EnumUpTo(LineStyle::Outset);
            for (Color& c : v.colors) c = ReadColor(r);
            list->items.push_back(v);
            break;
          }
          case 2: {
            DrawGlyphs v;
            const uint64_t faceId = r.U64();
            const auto face = s.faces.find(faceId);
            v.face = face == s.faces.end() ? nullptr : face->second;
            v.size = r.F32(); v.color = ReadColor(r); v.x = r.F32(); v.y = r.F32();
            const uint32_t glyphs = r.Count(4);
            v.glyphs.reserve(glyphs);
            for (uint32_t g = 0; g < glyphs && r.ok(); ++g) v.glyphs.push_back(r.U32());
            const uint32_t offsets = r.Count(4);
            v.offsets.reserve(offsets);
            for (uint32_t g = 0; g < offsets && r.ok(); ++g) v.offsets.push_back(r.F32());
            const uint32_t shadows = r.Count(20);
            for (uint32_t g = 0; g < shadows && r.ok(); ++g) v.shadows.push_back(ReadShadow(r));
            // (The painter takes a glyph and its place from the two: they are as many.)
            if (v.offsets.size() < v.glyphs.size() * 2) v.glyphs.clear();
            list->items.push_back(std::move(v));
            break;
          }
          case 3: { PushClip v; v.rect = ReadRounded(r); list->items.push_back(v); break; }
          case 4: list->items.push_back(PopClip{}); break;
          case 5: { PushLayer v; v.opacity = r.F32(); list->items.push_back(v); break; }
          case 6: list->items.push_back(PopLayer{}); break;
          case 7: {
            PushTransform v;
            for (float& f : v.matrix.m) f = r.F32();
            v.followsScroll = r.Bool();
            v.sticky = ReadSticky(r);
            list->items.push_back(v);
            break;
          }
          case 8: list->items.push_back(PopTransform{}); break;
          case 9: { DrawImageLayer v; layer(v.layer); list->items.push_back(std::move(v)); break; }
          case 10: {
            DrawImage v;
            v.image = image(r.U64());
            v.clip = ReadRounded(r);
            v.fit = r.EnumUpTo(ObjectFit::ScaleDown);
            v.x = ReadLength(r); v.y = ReadLength(r);
            v.pixelated = r.Bool();
            list->items.push_back(std::move(v));
            break;
          }
          case 11: {
            DrawVideo v;
            const uint64_t id = r.U64();
            if (id) {
              auto& video = s.videos[id];
              if (!video) video = std::make_shared<RemoteVideo>();
              v.player = video;
            }
            v.clip = ReadRounded(r);
            v.fit = r.EnumUpTo(ObjectFit::ScaleDown);
            v.x = ReadLength(r); v.y = ReadLength(r);
            v.poster = image(r.U64());
            v.controls = r.Bool();
            v.captions = r.String(1 << 16);
            list->items.push_back(std::move(v));
            break;
          }
          case 12: {
            DrawCanvas v;
            const uint64_t id = r.U64();
            if (id) {
              auto& canvas = s.canvases[id];
              if (!canvas) canvas = std::make_shared<RemoteCanvas>();
              v.surface = canvas;
            }
            v.clip = ReadRounded(r);
            v.fit = r.EnumUpTo(ObjectFit::ScaleDown);
            v.x = ReadLength(r); v.y = ReadLength(r);
            v.pixelated = r.Bool();
            list->items.push_back(std::move(v));
            break;
          }
          case 13: list->items.push_back(PushMask{}); break;
          case 14: {
            PopMask v;
            const uint32_t layers = r.Count(40);
            if (layers > 64) return false;
            for (uint32_t l = 0; l < layers && r.ok(); ++l) {
              ImageLayer one;
              layer(one);
              v.layers.push_back(std::move(one));
            }
            list->items.push_back(std::move(v));
            break;
          }
          case 15: { DrawBoxShadow v; v.shape = ReadRounded(r); v.shadow = ReadShadow(r); list->items.push_back(v); break; }
          case 16: {
            DrawOutline v;
            v.shape = ReadRounded(r);
            v.offset = r.F32(); v.width = r.F32();
            v.style = r.EnumUpTo(LineStyle::Outset);
            v.color = ReadColor(r);
            list->items.push_back(v);
            break;
          }
          default: return false;
        }
      }
      if (!r.ok()) return false;
      s.current = std::move(list);
      return true;
    }
  }
  return false;
}

}  // namespace solar::paint
