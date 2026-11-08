// The messages between the window and the page, and the memory they share: what is written is read back, what is not valid is not taken, garbage ends nothing but the message it is,
// and a buffer sent as a descriptor arrives as memory both ends see (sealed: it cannot be made smaller).

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <thread>

#include "solar/gui/FrameBuffer.h"
#include "solar/gui/GpuWire.h"
#include "solar/gui/Wire.h"
#include "solar/net/Ipc.h"

using namespace solar::gui;
using solar::net::ipc::Channel;
using solar::net::ipc::Reader;
using solar::net::ipc::Writer;

namespace {

int total = 0, failed = 0;
void Check(const std::string& name, bool ok) {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s\n", name.c_str());
}

template <typename T>
std::string Encode(const T& value) {
  Writer out;
  wire::Write(out, value);
  return out.Take();
}

}  // namespace

int main() {
  {
    wire::Geometry g{1280, 800, 2560, 1600, 2.f};
    const std::string bytes = Encode(g);
    Reader in(bytes);
    wire::Geometry back;
    Check("geometry round trip", wire::Read(in, back) && back.width == 1280 && back.pixelHeight == 1600 && back.density == 2.f && in.AtEnd());
    for (const wire::Geometry& bad : {wire::Geometry{0, 800, 1, 1, 1}, wire::Geometry{1, 1, 1, 1000000, 1}, wire::Geometry{1, 1, 1, 1, 0.f}, wire::Geometry{-5, 1, 1, 1, 1}}) {
      const std::string b = Encode(bad);
      Reader r(b);
      wire::Geometry out;
      Check("geometry out of range refused", !wire::Read(r, out));
    }
  }
  {
    wire::Input input;
    input.kind = wire::Input::Kind::KeyDown;
    input.key = 0x61;
    input.mod = 3;
    input.repeat = true;
    input.button = 2;
    input.x = 10.5f;
    input.y = -4.f;
    input.xrel = 1;
    input.yrel = 2;
    input.text = "h\xc3\xa9llo";
    input.clipboard = std::string("pasted\0text", 11);
    const std::string bytes = Encode(input);
    Reader in(bytes);
    wire::Input back;
    Check("input round trip", wire::Read(in, back) && back.kind == input.kind && back.key == 0x61 && back.mod == 3 && back.repeat && back.button == 2 && back.x == 10.5f && back.y == -4.f && back.text == input.text && back.clipboard == input.clipboard);
    // A kind that is none, a number that is not one, a text cut short.
    std::string broken = bytes;
    broken[0] = 99;
    Reader r1(broken);
    wire::Input out;
    Check("input with no such kind refused", !wire::Read(r1, out));
    Reader r2(std::string_view(bytes).substr(0, bytes.size() - 3));
    Check("input cut short refused", !wire::Read(r2, out));
    wire::Input weird = input;
    weird.x = std::numeric_limits<float>::quiet_NaN();
    weird.y = std::numeric_limits<float>::infinity();
    const std::string wb = Encode(weird);
    Reader r3(wb);
    Check("input with NaN and infinity reads as zero", wire::Read(r3, out) && out.x == 0 && out.y == 0);
  }
  {
    wire::NewBuffer b{3, 100, 50, 400};
    const std::string bytes = Encode(b);
    Reader in(bytes);
    wire::NewBuffer back;
    Check("buffer round trip", wire::Read(in, back) && back.index == 3 && back.stride == 400);
    for (const wire::NewBuffer& bad : {wire::NewBuffer{64, 1, 1, 4}, wire::NewBuffer{0, 100, 1, 399}, wire::NewBuffer{0, 0, 1, 4}, wire::NewBuffer{0, 20000, 1, 80000}, wire::NewBuffer{0, 10, 10, -40}}) {
      const std::string b2 = Encode(bad);
      Reader r(b2);
      wire::NewBuffer out;
      Check("buffer out of range refused", !wire::Read(r, out));
    }
    wire::Frame f{2, 99};
    const std::string fb = Encode(f);
    Reader fr(fb);
    wire::Frame fback;
    Check("frame round trip", wire::Read(fr, fback) && fback.index == 2 && fback.serial == 99);
    wire::Frame badFrame{200, 1};
    const std::string bf = Encode(badFrame);
    Reader br(bf);
    Check("frame with a buffer that is none refused", !wire::Read(br, fback));
  }
  {
    // Garbage ends nothing but the message it is.
    std::mt19937 random(7);
    int accepted = 0;
    for (int i = 0; i < 5000; ++i) {
      std::string junk(random() % 80, '\0');
      for (char& c : junk) c = static_cast<char>(random());
      Reader a(junk), b(junk), c(junk), d(junk);
      wire::Geometry g;
      wire::Input in;
      wire::NewBuffer nb;
      wire::Frame fr;
      accepted += wire::Read(a, g) + wire::Read(b, in) + wire::Read(c, nb) + wire::Read(d, fr);
    }
    Check("garbage was read without a crash", accepted >= 0);
  }
  {
    // A buffer goes as a descriptor: the other end sees what is written, and its map cannot be written.
    auto [a, b] = Channel::MakePair();
    Channel page(a), window(b);
    SharedMemory memory;
    Check("memory made", memory.Create(4096));
    std::memcpy(memory.data(), "pixels!", 8);
    Check("buffer sent with its descriptor", page.SendRaw(static_cast<uint8_t>(wire::ToBrowser::NewBuffer), "meta", memory.fd()));
    Check("message without a descriptor", page.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Quit), ""));
    uint8_t type = 0;
    std::string bytes;
    int fd = -1;
    Check("received with a descriptor", window.ReceiveRaw(type, bytes, &fd) && type == static_cast<uint8_t>(wire::ToBrowser::NewBuffer) && bytes == "meta" && fd >= 0);
    SharedMemory mapped;
    Check("descriptor mapped", mapped.Adopt(fd, 4096, false));
    Check("window sees the pixels", mapped.data() && std::memcmp(mapped.data(), "pixels!", 8) == 0);
    std::memcpy(memory.data(), "later!!", 8);
    Check("window sees a later change", std::memcmp(mapped.data(), "later!!", 8) == 0);
    int none = 5;
    Check("next message has no descriptor", window.ReceiveRaw(type, bytes, &none) && type == static_cast<uint8_t>(wire::ToBrowser::Quit) && none == -1);
    // Whoever has the memory cannot make it smaller (the reader would fault).
    Check("memory cannot be shrunk", ::ftruncate(memory.fd(), 10) != 0 && ::ftruncate(mapped.fd(), 10) != 0);
    // Memory that is not sealed is not taken.
    const int plain = ::memfd_create("plain", MFD_CLOEXEC);
    Check("plain memory made", plain >= 0 && ::ftruncate(plain, 4096) == 0);
    SharedMemory unsealed;
    Check("memory not sealed is refused", !unsealed.Adopt(plain, 4096, false));
    // Memory smaller than it was said to be is not mapped.
    SharedMemory small;
    Check("small memory made", small.Create(100));
    SharedMemory liar;
    const int copy = ::dup(small.fd());
    Check("memory smaller than said is refused", !liar.Adopt(copy, 1 << 20, false));
    // A descriptor nobody asked for is closed.
    SharedMemory third;
    Check("third made", third.Create(64));
    page.SendRaw(1, "x", third.fd());
    Check("descriptor not asked for is dropped", window.ReceiveRaw(type, bytes) && bytes == "x");
  }
  {
    // What a page and the GPU's process say to each other.
    solar::gui::gpu::Draw draw;
    draw.index = 2; draw.serial = 77; draw.pixelWidth = 2560; draw.pixelHeight = 1600; draw.density = 2.f; draw.scrollX = 1.5; draw.scrollY = 1234.25;
    draw.barVisible = true; draw.barEmphasized = true; draw.barAlpha = 0.5f; draw.barTop = 10; draw.barHeight = 40; draw.windowWidth = 1280; draw.windowHeight = 800;
    Writer out;
    solar::gui::gpu::Write(out, draw);
    const std::string bytes = out.Take();
    Reader in(bytes);
    solar::gui::gpu::Draw back;
    Check("a draw round trip", solar::gui::gpu::Read(in, back) && back.index == 2 && back.serial == 77 && back.pixelWidth == 2560 && back.scrollY == 1234.25 && back.barVisible && back.barEmphasized && !back.barDragging && back.barAlpha == 0.5f && back.windowHeight == 800);
    for (const solar::gui::gpu::Draw& bad : {solar::gui::gpu::Draw{64, 1, 10, 10, 1.f}, solar::gui::gpu::Draw{0, 1, 0, 10, 1.f}, solar::gui::gpu::Draw{0, 1, 10, 100000, 1.f}, solar::gui::gpu::Draw{0, 1, 10, 10, 0.f}, solar::gui::gpu::Draw{0, 1, 10, 10, 99.f}}) {
      Writer w;
      solar::gui::gpu::Write(w, bad);
      const std::string b = w.Take();
      Reader r(b);
      solar::gui::gpu::Draw out2;
      Check("a draw out of range is refused", !solar::gui::gpu::Read(r, out2));
    }
    Writer wb;
    solar::gui::gpu::Write(wb, solar::gui::gpu::Buffer{3, 100, 50, 400});
    const std::string bb = wb.Take();
    Reader rb(bb);
    solar::gui::gpu::Buffer buffer;
    Check("a buffer round trip", solar::gui::gpu::Read(rb, buffer) && buffer.index == 3 && buffer.stride == 400);
    Reader cut(std::string_view(bytes).substr(0, bytes.size() - 5));
    Check("a draw cut short is refused", !solar::gui::gpu::Read(cut, back));
  }
  std::printf("wire: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
