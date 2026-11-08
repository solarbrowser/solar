/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

// What the threads of the page's process (the renderer) tell each other: the thread of the page (scripts, style, layout, the display list), and the compositor (the scroll, the scrollbar, the
// drawing of the display list into pictures the window shows). The window is in another process; it speaks to the compositor through the wire (solar/gui/Wire.h).

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <thread>

#include "solar/gui/Wire.h"
#include "solar/paint/DisplayList.h"

namespace solar::gui {

using Clock = std::chrono::steady_clock;

// What the user did, as the page has it (SDL's own event, its text and the clipboard it may paste kept here: the event's pointers do not outlive it).
struct UiEvent {
  SDL_Event event{};
  std::string text;  // a text input's text
  std::string clipboard;  // what the clipboard held when a key that pastes was pressed
  int mods = -1;  // the modifier keys (SDL's bits) at the time, where the window said
};

// A page in a frame that has more than its room: the wheel over it scrolls it, and the window has to know where it is to leave the page alone.
// (The place is the frame's content box, in the coordinates of the page, or of the window for what is held in the viewport.)
struct ScrollRegion {
  double x = 0, y = 0, width = 0, height = 0;
  bool fixed = false;
  double curX = 0, curY = 0, maxX = 0, maxY = 0;
};

struct Frame {
  std::shared_ptr<const solar::paint::DisplayList> list;
  double maxX = 0, maxY = 0;  // how far the page can be scrolled
  std::vector<ScrollRegion> regions;  // the frames that can be scrolled, the outer ones first
};

// The pointer is locked to an element of the page (the Pointer Lock API): the window hides the pointer and gives movements, and the page's mouse events are the locked element's.
inline std::atomic<bool> g_pointerLocked{false};

struct UiCommand {
  enum class Kind { Title, ScrollBy, ScrollTo, Forget, Shot, Quit, Clipboard, Fullscreen, PointerLock } kind = Kind::Title;
  std::string text;
  double x = 0, y = 0;
  bool instant = false;
};

class Shared {
 public:
  // The window, as the thread of the page needs to know it (in points, and the pixels they come to).
  std::atomic<int> width{1280}, height{800}, pixelWidth{1280}, pixelHeight{800};
  std::atomic<float> density{1.f};
  std::atomic<bool> quit{false}, pageDone{false};
  // The scroll of the page as the window has it; the serial moves with each change.
  std::atomic<double> scrollX{0}, scrollY{0};
  std::atomic<uint64_t> scrollSerial{0};
  std::atomic<bool> gliding{false};
  std::atomic<int64_t> scrollInputNs{0};  // when the user last asked for scrolling (the steady clock)

  // The window to the page.
  void PushEvent(UiEvent event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events_.push_back(std::move(event));
    }
    cv_.notify_one();
  }
  // The next event, waiting at most `wait` for one.
  bool WaitEvent(UiEvent& out, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (events_.empty() && wait.count() > 0) cv_.wait_for(lock, wait, [&] { return !events_.empty() || quit.load(); });
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
  }
  // The page to the window.
  void PostFrame(Frame frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_ = std::move(frame);
      hasFrame_ = true;
    }
    WakeWindow();
  }
  bool TakeFrame(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_) return false;
    out = std::move(frame_);
    hasFrame_ = false;
    return true;
  }
  void Post(UiCommand command) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      commands_.push_back(std::move(command));
    }
    WakeWindow();
  }
  std::vector<UiCommand> TakeCommands() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UiCommand> out(commands_.begin(), commands_.end());
    commands_.clear();
    return out;
  }
  // What the window (the other process) tells, and the page's own test scripts, as the compositor has it to take in.
  struct CompositorInput {
    enum class Kind { Event, Geometry, Released } kind = Kind::Event;
    UiEvent event;
    solar::gui::wire::Geometry geometry;
    uint32_t index = 0;
  };
  void PushCompositor(CompositorInput input) {
    {
      std::lock_guard<std::mutex> lock(compositorMutex_);
      compositorInput_.push_back(std::move(input));
    }
    WakeWindow();
  }
  std::vector<CompositorInput> TakeCompositor() {
    std::lock_guard<std::mutex> lock(compositorMutex_);
    std::vector<CompositorInput> out(std::make_move_iterator(compositorInput_.begin()), std::make_move_iterator(compositorInput_.end()));
    compositorInput_.clear();
    return out;
  }
  // A test script's event (what the page makes up to drive itself): it goes where the window's do.
  void Inject(const SDL_Event& event) {
    CompositorInput input;
    input.event.event = event;
    if (event.type == SDL_EVENT_TEXT_INPUT) input.event.text = event.text.text ? event.text.text : "";
    PushCompositor(std::move(input));
  }
  // Wakes the compositor (it waits for this, or for a time to pass).
  void WakeWindow() {
    {
      std::lock_guard<std::mutex> lock(compositorMutex_);
      compositorWoken_ = true;
    }
    compositorCv_.notify_all();
  }
  void WaitCompositor(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(compositorMutex_);
    if (wait.count() > 0) compositorCv_.wait_for(lock, wait, [&] { return compositorWoken_; });
    compositorWoken_ = false;
  }
  // The connections to the browser: its network (the descriptor), and the one the window's messages come on (navigations are asked on it and answered on it).
  int networkFd = -1;
  int gpuFd = -1;  // the connection to the GPU's process, if the page has one
  bool sandboxed = false;  // the process is shut in (what it may not use, it does not)
  solar::net::ipc::Channel* browserLink = nullptr;
  // Where the answers to navigations go (the page sets it when it is ready for them; what comes before is not kept: nothing was asked yet).
  void SetNavigationSink(std::function<void(solar::gui::wire::ToRenderer, std::string)> sink) {
    std::lock_guard<std::mutex> lock(navigationMutex_);
    navigationSink_ = std::move(sink);
  }
  void DeliverNavigation(solar::gui::wire::ToRenderer type, std::string bytes) {
    std::lock_guard<std::mutex> lock(navigationMutex_);
    if (navigationSink_) navigationSink_(type, std::move(bytes));
  }
  solar::gui::wire::Geometry initialGeometry;  // (what the window said first: the page starts knowing it)
  std::atomic<int> mods{0};  // the modifier keys (SDL's bits) as the last thing the user did had them
  void WakePage() { cv_.notify_all(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<UiEvent> events_;
  std::deque<UiCommand> commands_;
  Frame frame_;
  bool hasFrame_ = false;
  std::mutex navigationMutex_;
  std::function<void(solar::gui::wire::ToRenderer, std::string)> navigationSink_;
  std::mutex compositorMutex_;
  std::condition_variable compositorCv_;
  std::deque<CompositorInput> compositorInput_;
  bool compositorWoken_ = false;
};
inline int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

// The compositor (on the thread that calls it) until the program ends; `link` is the connection to the window.
void RunCompositor(Shared& shared, net::ipc::Channel& link, int gpuFd = -1);
// Takes what the window says (its size, what the user does, the pictures it has let go of) and gives it to the compositor; the thread ends when the connection does.
std::thread StartWindowLink(Shared& shared, net::ipc::Channel& link);

}  // namespace solar::gui
