/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solar/media/Player.h"

// Media Source Extensions (https://www.w3.org/TR/media-source-2/): the page makes the media itself, in pieces. A MediaSource has SourceBuffers; the bytes appended to one
// (the init segment of an fMP4 or WebM, then its media segments) are demuxed as they come into packets that are kept by time, until the page removes them; a Player that is
// made over the MediaSource decodes from those packets at the position of its clock, and stalls (waiting) where there are none. Seeking is the page's: it removes what
// it does not want and appends from where it is going.
namespace solar::media {

struct MseCodec;
struct MsePacket;
class MseBuffer;

class MseSource {
 public:
  // `changed` is called, from any thread, when a buffer finished an append (or failed), for the script to look at.
  static std::shared_ptr<MseSource> Create(std::function<void()> changed);
  ~MseSource();

  // A buffer for a type ("video/mp4; codecs=..."), or null if it is not one that can be played.
  std::shared_ptr<MseBuffer> AddBuffer(const std::string& mime);
  void RemoveBuffer(const std::shared_ptr<MseBuffer>& buffer);
  void SetDuration(double seconds);
  double duration() const;
  void EndOfStream();
  void Reopen();
  bool ended() const;
  // What is buffered in all the tracks (where each has something).
  std::vector<TimeRange> Buffered();

  // For the player: the packet after `after` (a time in seconds) of the kind (0 video, 1 audio): false if there is none, or if the next one is past a gap of a second or
  // more (`gap` says which). FindStart: where a seek to `target` begins, which has to be buffered.
  bool Peek(int kind, double after, MsePacket& out, bool& gap);
  bool FindStart(int kind, double target, double& dts);
  bool HasTrack(int kind);
  std::shared_ptr<MseCodec> CodecOf(int kind);
  // A number that changes whenever packets were added or removed.
  uint64_t version() const;
  void SetWake(std::function<void()> wake);

  struct Impl;

 private:
  MseSource();
  std::unique_ptr<Impl> impl_;
  friend class MseBuffer;
};

class MseBuffer {
 public:
  ~MseBuffer();
  // False when it would be over the quota (the page is to remove what it does not need and try again). The parsing is done on a thread of its own: updating() is true until
  // it has taken all that was appended.
  bool Append(const uint8_t* data, size_t size);
  bool updating() const;
  // Whether the data could not be parsed (an error event).
  bool failed() const;
  void Abort();
  void Remove(double start, double end);
  void SetTimestampOffset(double seconds);
  std::vector<TimeRange> Buffered(int kind = -1);
  bool HasTrack(int kind);
  size_t bytes() const;

  struct Impl;

 private:
  friend class MseSource;
  MseBuffer();
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::media
