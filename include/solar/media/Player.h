/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Playing media (https://html.spec.whatwg.org/multipage/media.html): a Player takes the bytes of a resource as they arrive, finds the tracks in them with FFmpeg's
// libavformat, decodes with libavcodec, and plays: the sound goes to the program's audio output, the pictures are kept for whoever draws the element, and the clock
// that decides which picture is the current one is the sound's (or, with none, the wall's). A Player runs on a thread of its own; what it has to tell is a state
// to read and events to take, and a callback that says there is something new (it may be called from any thread).
namespace solar::media {

// The bytes of a resource, as they come. The network side appends; the player reads (and waits for what has not come yet).
class ByteStream {
 public:
  void Append(const uint8_t* data, size_t size);
  // The end: `failed` if the transfer broke off.
  void Finish(bool failed);
  // The size the server announced (Content-Length), or 0.
  void SetTotal(uint64_t total);

  // For the reader: up to `size` bytes from `offset`, waiting until there are some or the stream is at its end (0), `interrupted` says to stop (-1), the wait has gone on
  // for `timeoutMs` (-2), or the stream broke off (-3).
  int64_t Read(uint64_t offset, uint8_t* out, size_t size, const std::function<bool()>& interrupted, int timeoutMs = 1000000000);
  // The size, once it is known (when the stream is finished, or announced), or -1.
  int64_t Size() const;
  // Waits until the stream is finished (or interrupted).
  bool WaitForEnd(const std::function<bool()>& interrupted);
  uint64_t available() const;
  uint64_t total() const;
  bool finished() const;
  bool failed() const;
  // Wakes a reader that is waiting, so that it looks at what it is waiting on again.
  void Poke();

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string bytes_;
  uint64_t total_ = 0;
  bool finished_ = false, failed_ = false;
};

// One picture of a video, as the sink draws it: B, G, R, A in a byte each (the byte order of the platform's N32 on little-endian machines), opaque.
struct VideoFrame {
  int width = 0, height = 0;  // in pixels
  std::vector<uint8_t> bgra;
  double pts = 0;
  uint64_t serial = 0;  // which picture this is (it changes with each)
};

// What plays the sound. The program gives the factory (the GUI has SDL's); with none the media plays without sound.
class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  // Opens a playback stream of float samples, interleaved, `channels` of them to a frame.
  virtual bool Start(int sampleRate, int channels) = 0;
  virtual void Queue(const float* samples, size_t frames) = 0;
  // Frames given and not yet played.
  virtual size_t QueuedFrames() = 0;
  virtual void Pause(bool paused) = 0;
  // Forgets what was given and not played.
  virtual void Clear() = 0;
};
using AudioOutputFactory = std::function<std::unique_ptr<AudioOutput>()>;
void SetAudioOutputFactory(AudioOutputFactory factory);

// https://html.spec.whatwg.org/multipage/media.html#ready-states
enum ReadyState { kHaveNothing = 0, kHaveMetadata = 1, kHaveCurrentData = 2, kHaveFutureData = 3, kHaveEnoughData = 4 };

struct TimeRange {
  double start = 0, end = 0;
};

struct PlayerState {
  int readyState = kHaveNothing;
  double duration = 0;  // NaN until known
  int videoWidth = 0, videoHeight = 0;  // as displayed (the sample aspect ratio applied)
  bool hasVideo = false, hasAudio = false;
  double currentTime = 0;
  bool seeking = false, ended = false, stalled = false;
  int error = 0;  // MEDIA_ERR_*: 0 none, 2 network, 3 decode, 4 not supported
  std::string errorMessage;
  std::vector<TimeRange> buffered;
};

class Player {
 public:
  // Starts playing from the stream (paused). `changed` is called, from any thread, when the state or the events have something new.
  static std::shared_ptr<Player> Create(std::shared_ptr<ByteStream> stream, std::function<void()> changed);
  ~Player();
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  void Play();
  void Pause();
  void Seek(double seconds);
  void SetVolume(double volume);
  void SetMuted(bool muted);
  void SetRate(double rate);
  void SetLoop(bool loop);

  PlayerState State();
  // The events there have been since the last call, by the names of the media events ("loadedmetadata", "canplay", "seeked", "ended", ...), in order.
  std::vector<std::string> TakeEvents();
  // The picture to show now (the clock decides), or null if there is none yet. Cheap; call it when drawing.
  std::shared_ptr<const VideoFrame> Frame();
  // Whether the player is playing (the program keeps drawing while one is).
  bool playing() const { return playing_.load(); }

  struct Impl;

 private:
  Player();
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> playing_{false};
  friend struct Impl;
};

// Whether FFmpeg has what plays the type: 0 for no, 1 for maybe, 2 for probably (the strings canPlayType gives are "", "maybe" and "probably").
int CanPlayType(const std::string& mime, const std::string& codecs);

// The picture that was last drawn and the number of pictures that are playing (the window keeps drawing while there are some).
int PlayingCount();
uint64_t FrameCounter();  // changes when any player has a new picture

// The player of an element (the document's video or audio), for layout and painting to look at; set by the script's natives.
void AttachPlayer(const void* element, std::shared_ptr<Player> player);
void DetachPlayer(const void* element);
std::shared_ptr<Player> PlayerOf(const void* element);

}  // namespace solar::media
