// A display list as bytes and back draws the same: every kind of item, the fonts (by path and by bytes, a variation), pictures, a video, a canvas, a gradient, a mask; what is let go of;
// and what is not a list is not taken.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "../src/paint/CanvasSurface.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/core/SkStream.h"
#include "solar/font/Face.h"
#include "solar/paint/ListCodec.h"
#include "../src/gui/GpuRaster.h"
#include "solar/paint/SkiaPainter.h"

using namespace solar::paint;

namespace {
int total = 0, failed = 0;
void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

SkBitmap Render(const DisplayList& list, double sx = 0, double sy = 0) {
  SkPaint paint;
  auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 300));
  surface->getCanvas()->clear(SK_ColorWHITE);
  SkiaPainter painter;
  painter.Draw(list, *surface->getCanvas(), sx, sy);
  SkBitmap bitmap;
  bitmap.allocPixels(SkImageInfo::MakeN32Premul(400, 300));
  surface->readPixels(bitmap, 0, 0);
  return bitmap;
}

bool Same(const SkBitmap& a, const SkBitmap& b) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  return std::memcmp(a.getPixels(), b.getPixels(), a.computeByteSize()) == 0;
}

std::string PngOf(int w, int h, SkColor color) {
  SkBitmap bitmap;
  bitmap.allocPixels(SkImageInfo::MakeN32Premul(w, h));
  bitmap.eraseColor(color);
  for (int i = 0; i < std::min(w, h); ++i) *bitmap.getAddr32(i, i) = SK_ColorBLACK;
  SkDynamicMemoryWStream out;
  SkPngEncoder::Encode(&out, bitmap.pixmap(), SkPngEncoder::Options());
  sk_sp<SkData> data = out.detachAsData();
  return std::string(static_cast<const char*>(data->data()), data->size());
}

struct FakeVideo : solar::media::VideoSource {
  solar::media::PlayerState state;
  std::shared_ptr<const solar::media::VideoFrame> frame;
  solar::media::PlayerState State() override { return state; }
  std::shared_ptr<const solar::media::VideoFrame> Frame() override { return frame; }
};

struct FakeCanvas : CanvasSurface {
  FakeCanvas() : CanvasSurface(1, 1) {}
  sk_sp<SkImage> Latest() const override { return image; }
  sk_sp<SkImage> image;
};

RoundedRect Box(float x, float y, float w, float h, float r = 0) {
  RoundedRect box;
  box.rect = {x, y, w, h};
  for (float& v : box.radii) v = r;
  return box;
}

// What carries the list across: encode, then decode all the messages in order.
std::shared_ptr<const DisplayList> Carry(ListEncoder& encoder, ListDecoder& decoder, const DisplayList& list, bool dynamic = true) {
  for (const EncodedMessage& message : encoder.Encode(list)) {
    if (!decoder.Apply(message.type, message.bytes)) return nullptr;
  }
  if (dynamic) {
    for (const EncodedMessage& message : encoder.Dynamic()) {
      if (!decoder.Apply(message.type, message.bytes)) return nullptr;
    }
  }
  return decoder.list();
}
}  // namespace

int main() {
  std::shared_ptr<solar::font::Face> byPath;
  for (const char* path : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
    byPath = solar::font::Face::OpenFile(path);
    if (byPath) break;
  }
  if (!byPath) {
    std::printf("list codec: no font to try with (skipped)\n");
    return 0;
  }
  std::ifstream in(byPath->SourcePath(), std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::shared_ptr<solar::font::Face> byBytes = solar::font::Face::Open(buffer.str());
  Check("a face opened from a file knows its file and one made of bytes does not", !byPath->SourcePath().empty() && byBytes && byBytes->SourcePath().empty());

  auto png = std::make_shared<const ImageData>(ImageData{std::make_shared<const std::string>(PngOf(30, 20, SK_ColorRED)), "x"});
  auto video = std::make_shared<FakeVideo>();
  {
    auto frame = std::make_shared<solar::media::VideoFrame>();
    frame->width = 8; frame->height = 4; frame->serial = 3;
    frame->bgra.assign(8 * 4 * 4, 0);
    for (size_t i = 0; i < frame->bgra.size(); i += 4) { frame->bgra[i] = 255; frame->bgra[i + 3] = 255; }
    video->frame = frame;
    video->state.videoWidth = 8;
    video->state.videoHeight = 4;
    video->state.playing = true;
    video->state.duration = 100;
    video->state.currentTime = 30;
    video->state.controlsVisible = true;
  }
  auto canvas = std::make_shared<FakeCanvas>();
  {
    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(10, 10));
    bitmap.eraseColor(SK_ColorBLUE);
    canvas->image = bitmap.asImage();
  }
  auto gradient = std::make_shared<Gradient>();
  gradient->stops = {{Color{255, 0, 0, 255}, {0, 0, false}, false}, {Color{0, 0, 255, 255}, {100, 0, false}, true}};

  DisplayList list;
  list.scrollX = 0; list.scrollY = 0; list.roomY = 100; list.width = 400; list.height = 600;
  list.items.push_back(FillRoundedRect{Box(10, 10, 100, 50, 8), Color{20, 160, 60, 255}});
  { DrawBorder b; b.outer = Box(10, 70, 100, 40, 6); b.inner = Box(14, 74, 92, 32, 3); for (int i = 0; i < 4; ++i) { b.widths[i] = 4; b.styles[i] = LineStyle::Solid; b.colors[i] = Color{200, 50, 50, 255}; } list.items.push_back(b); }
  { DrawGlyphs g; g.face = byPath; g.size = 18; g.color = Color{0, 0, 0, 255}; g.x = 130; g.y = 30; for (uint32_t i = 0; i < 5; ++i) { g.glyphs.push_back(byPath->GlyphFor(U'a' + i)); g.offsets.push_back(i * 12.f); g.offsets.push_back(0); } g.shadows.push_back(Shadow{Color{0, 0, 0, 120}, 1, 1, 2, 0, false}); list.items.push_back(g); }
  { DrawGlyphs g; g.face = byBytes; g.size = 14; g.color = Color{0, 0, 120, 255}; g.x = 130; g.y = 60; for (uint32_t i = 0; i < 4; ++i) { g.glyphs.push_back(byBytes->GlyphFor(U'm' + i)); g.offsets.push_back(i * 10.f); g.offsets.push_back(0); } list.items.push_back(g); }
  list.items.push_back(PushClip{Box(200, 10, 60, 60, 10)});
  list.items.push_back(FillRoundedRect{Box(190, 0, 100, 100), Color{0, 0, 200, 255}});
  list.items.push_back(PopClip{});
  list.items.push_back(PushLayer{0.5f});
  list.items.push_back(FillRoundedRect{Box(230, 40, 60, 60), Color{255, 0, 0, 255}});
  list.items.push_back(PopLayer{});
  { PushTransform t; t.matrix.m[12] = 5; t.matrix.m[13] = 120; list.items.push_back(t); list.items.push_back(FillRoundedRect{Box(0, 0, 30, 30), Color{0, 0, 0, 255}}); list.items.push_back(PopTransform{}); }
  { DrawImage d; d.image = png; d.clip = Box(10, 130, 60, 40); d.fit = ObjectFit::Contain; list.items.push_back(d); }
  { DrawImageLayer d; d.layer.image = png; d.layer.clip = Box(80, 130, 60, 40); d.layer.origin = {80, 130, 60, 40}; d.layer.repeatX = Repeat::Repeat; d.layer.repeatY = Repeat::NoRepeat; list.items.push_back(d); }
  { DrawImageLayer d; d.layer.gradient = gradient; d.layer.clip = Box(150, 130, 100, 40); d.layer.origin = {150, 130, 100, 40}; list.items.push_back(d); }
  { DrawBoxShadow s; s.shape = Box(270, 130, 60, 40, 6); s.shadow = Shadow{Color{0, 0, 0, 160}, 3, 3, 6, 1, false}; list.items.push_back(s); list.items.push_back(FillRoundedRect{Box(270, 130, 60, 40, 6), Color{250, 250, 250, 255}}); }
  { DrawOutline o; o.shape = Box(10, 190, 60, 30); o.offset = 2; o.width = 2; o.color = Color{255, 140, 0, 255}; list.items.push_back(o); }
  { DrawVideo v; v.player = video; v.clip = Box(90, 190, 100, 60); v.controls = true; v.poster = png; v.captions = "hello"; list.items.push_back(v); }
  { DrawCanvas c; c.surface = canvas; c.clip = Box(210, 190, 40, 40); list.items.push_back(c); }
  list.items.push_back(PushMask{});
  list.items.push_back(FillRoundedRect{Box(270, 190, 100, 60), Color{0, 128, 0, 255}});
  { PopMask m; ImageLayer l; l.gradient = gradient; l.clip = Box(270, 190, 100, 60); l.origin = {270, 190, 100, 60}; m.layers.push_back(l); list.items.push_back(m); }

  const SkBitmap direct = Render(list);
  ListEncoder encoder;
  ListDecoder decoder;
  decoder.AllowFontDirectories({"/usr/share/fonts"});
  const auto carried = Carry(encoder, decoder, list);
  Check("the list is carried", carried != nullptr && carried->items.size() == list.items.size() && carried->width == 400 && carried->roomY == 100);
  if (carried) {
    const SkBitmap through = Render(*carried);
    Check("it draws the same through bytes as directly", Same(direct, through));
    const SkBitmap scrolled = Render(list, 0, 40), scrolledThrough = Render(*carried, 0, 40);
    Check("at another scroll too", Same(scrolled, scrolledThrough));
  }

  // What is sent once is not sent again, and what changed is.
  {
    const auto again = encoder.Encode(list);
    size_t definitions = 0;
    for (const EncodedMessage& m : again) if (m.type == ListMessage::DefineFace || m.type == ListMessage::DefineImage) ++definitions;
    Check("a list sent again has no definitions", definitions == 0);
    auto frame = std::make_shared<solar::media::VideoFrame>(*video->frame);
    frame->serial = 4;
    video->frame = frame;
    const auto dynamic = encoder.Dynamic();
    bool newFrame = false, state = false;
    for (const EncodedMessage& m : dynamic) { if (m.type == ListMessage::DefineBitmap) newFrame = true; if (m.type == ListMessage::VideoState) state = true; }
    Check("a new frame of a video is sent, and its state each time", newFrame && state);
    const auto quiet = encoder.Dynamic();
    bool frameAgain = false;
    for (const EncodedMessage& m : quiet) if (m.type == ListMessage::DefineBitmap) frameAgain = true;
    Check("a frame that was sent is not sent again", !frameAgain);
  }
  // What nothing has any more is let go of.
  {
    ListEncoder fresh;
    ListDecoder far;
    far.AllowFontDirectories({"/usr/share/fonts"});
    {
      auto temporary = std::make_shared<const ImageData>(ImageData{std::make_shared<const std::string>(PngOf(5, 5, SK_ColorGREEN)), ""});
      DisplayList small;
      DrawImage d;
      d.image = temporary;
      d.clip = Box(0, 0, 10, 10);
      small.items.push_back(d);
      Carry(fresh, far, small);
    }
    DisplayList empty;
    bool released = false;
    for (const EncodedMessage& m : fresh.Encode(empty)) if (m.type == ListMessage::Release) released = true;
    Check("a picture that nothing has is told to be let go of", released);
  }
  // A canvas that is a WebGL context's is said to be, not given: the list draws from what the resolver gives.
  {
    auto glCanvas = std::make_shared<FakeCanvas>();
    glCanvas->remoteGl = 7;
    DisplayList withGl;
    DrawCanvas c;
    c.surface = glCanvas;
    c.clip = Box(10, 10, 20, 20);
    withGl.items.push_back(c);
    ListEncoder glEncoder;
    ListDecoder glDecoder;
    uint32_t asked = 0;
    glDecoder.SetGlResolver([&](uint32_t context) -> sk_sp<SkImage> {
      asked = context;
      SkBitmap bitmap;
      bitmap.allocPixels(SkImageInfo::MakeN32Premul(4, 4));
      bitmap.eraseColor(SK_ColorMAGENTA);
      return bitmap.asImage();
    });
    bool said = false, given = false;
    for (const EncodedMessage& m : glEncoder.Encode(withGl)) {
      said |= m.type == ListMessage::DefineGlCanvas;
      Check("the message is taken", glDecoder.Apply(m.type, m.bytes));
    }
    for (const EncodedMessage& m : glEncoder.Dynamic()) given |= m.type == ListMessage::DefineBitmap;
    Check("a canvas of WebGL is said to be, and no pixels are given", said && !given);
    const auto carriedGl = glDecoder.list();
    const SkBitmap picture = carriedGl ? Render(*carriedGl) : SkBitmap();
    Check("it is drawn from what the resolver gives (and asked for that context)", carriedGl && asked == 7 && picture.width() > 0 && *picture.getAddr32(15, 15) == SK_ColorMAGENTA);
    size_t again = 0;
    for (const EncodedMessage& m : glEncoder.Encode(withGl)) if (m.type == ListMessage::DefineGlCanvas) ++again;
    Check("it is not said again", again == 0);
  }
  // What is not good is not taken.
  {
    ListDecoder strict;
    strict.AllowFontDirectories({"/usr/share/fonts"});
    // A font file that is not under the directories.
    {
      std::string bytes;
      for (int i = 0; i < 8; ++i) bytes.push_back(i == 0 ? 1 : 0);  // id 1
      bytes.push_back(1);                                           // by path
      const std::string path = "/etc/passwd";
      for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((path.size() >> (8 * i)) & 0xFF));
      bytes += path;
      for (int i = 0; i < 8; ++i) bytes.push_back(0);  // index, axes
      Check("a font by a path outside the directories is refused", !strict.Apply(ListMessage::DefineFace, bytes));
    }
    std::mt19937 random(5);
    int taken = 0;
    for (int i = 0; i < 4000; ++i) {
      std::string junk(random() % 120, '\0');
      for (char& c : junk) c = static_cast<char>(random());
      for (ListMessage type : {ListMessage::DefineFace, ListMessage::DefineImage, ListMessage::DefineBitmap, ListMessage::VideoState, ListMessage::Release, ListMessage::SetList, ListMessage::DefineGlCanvas}) taken += strict.Apply(type, junk);
    }
    Check("garbage is read without a crash", taken >= 0);
    // A list cut short.
    ListEncoder e2;
    const auto messages = e2.Encode(list);
    const EncodedMessage& last = messages.back();
    ListDecoder d2;
    d2.AllowFontDirectories({"/usr/share/fonts"});
    Check("a list cut short is refused", !d2.Apply(ListMessage::SetList, last.bytes.substr(0, last.bytes.size() / 2)) && d2.list() == nullptr);
  }
  // On the GPU (where the machine has one to use with no window): the list that was carried draws as it does on the CPU, but for what a GPU does otherwise at the edges.
  {
    std::string why;
    std::unique_ptr<solar::gui::GpuRaster> gpu = solar::gui::GpuRaster::Create(&why);
    if (!gpu) {
      std::printf("(no GPU for this test: %s)\n", why.c_str());
    } else {
      std::printf("(GPU: %s)\n", gpu->renderer().c_str());
      const auto onGpu = carried ? carried : nullptr;
      SkBitmap cpuPicture = Render(*onGpu);
      SkBitmap gpuPicture;
      gpuPicture.allocPixels(SkImageInfo::MakeN32Premul(400, 300));
      solar::paint::SkiaPainter painter;
      solar::gui::ScrollbarShape none;
      const bool drawn = gpu->Draw(painter, *onGpu, 400, 300, 1.f, 0, 0, none, static_cast<uint8_t*>(gpuPicture.getPixels()), gpuPicture.rowBytes());
      Check("the GPU draws the list", drawn);
      if (drawn) {
        size_t far = 0;
        for (int y = 0; y < 300; ++y) {
          for (int x = 0; x < 400; ++x) {
            const uint32_t a = *cpuPicture.getAddr32(x, y), b = *gpuPicture.getAddr32(x, y);
            for (int c = 0; c < 4; ++c) {
              const int d = static_cast<int>((a >> (8 * c)) & 0xFF) - static_cast<int>((b >> (8 * c)) & 0xFF);
              if (d > 48 || d < -48) { ++far; break; }
            }
          }
        }
        Check("and nearly as the CPU does (pixels far off: " + std::to_string(far) + " of 120000)", far < 3000);
        if (std::getenv("SOLAR_LISTCODEC_DUMP")) {
          for (const char* name : {"cpu", "gpu"}) {
            SkDynamicMemoryWStream out;
            SkPngEncoder::Encode(&out, (name[0] == 'c' ? cpuPicture : gpuPicture).pixmap(), SkPngEncoder::Options());
            sk_sp<SkData> data = out.detachAsData();
            std::ofstream file(std::string("/tmp/claude-1000/listcodec-") + name + ".png", std::ios::binary);
            file.write(static_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
          }
        }
      }
    }
  }
  std::printf("list codec: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
