/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "solar/paint/DisplayList.h"

// A display list as bytes, so that what draws it can be in another process (the GPU's): the list itself, and what it refers to that is not in it (the fonts, the pictures, a video's frames,
// a canvas) which is sent once, by a number, before the list that uses it, and let go of when no list has it. The sender is not trusted by the reader: what is read is checked.
namespace solar::paint {

// The kinds of message the encoder makes (and the decoder takes), each with bytes.
enum class ListMessage : uint8_t {
  DefineFace = 1,    // id, a path or the bytes of a font file, the index in it, the values of its axes
  DefineImage,       // id, the file of a picture as it came
  DefineBitmap,      // id, a picture of pixels (a video's frame, a canvas as it is): size, and B G R A bytes, premultiplied
  VideoState,        // id, the state of a video as the controls and the captions show it
  Release,           // ids that no list has any more
  SetList,           // the list
};

struct EncodedMessage {
  ListMessage type;
  std::string bytes;
};

class ListEncoder {
 public:
  // The messages that tell the other side of this list (the definitions of what it has not been told of, the list, and the numbers of what to let go of).
  std::vector<EncodedMessage> Encode(const DisplayList& list);
  // For each draw of the list that was last encoded: the frames of its videos that are new, their state, and the canvases that changed.
  std::vector<EncodedMessage> Dynamic();
  // The size, in bytes, of the largest message made (a font file or a frame).
  static constexpr size_t kMostPerMessage = size_t{48} << 20;

 private:
  struct Known {
    uint64_t id = 0;
    std::weak_ptr<const void> alive;
  };
  uint64_t IdOf(const void* key, const std::shared_ptr<const void>& owner, bool& isNew);
  uint64_t next_ = 1;
  std::unordered_map<const void*, Known> known_;
  // What the last list has that changes between draws.
  struct Video {
    uint64_t id;
    std::shared_ptr<media::VideoSource> source;
    uint64_t sentSerial = 0;
    bool sentAny = false;
  };
  struct Canvas {
    uint64_t id;
    std::shared_ptr<CanvasSurface> surface;
    uint32_t sentImage = 0;
  };
  std::vector<Video> videos_;
  std::vector<Canvas> canvases_;
  std::vector<uint64_t> live_;
};

class ListDecoder {
 public:
  ListDecoder();
  ~ListDecoder();
  // Takes a message; false where it is not one that is good (it is dropped, and the sender is not to be trusted with what it said).
  bool Apply(ListMessage type, const std::string& bytes);
  // The list that was last set (null before any), with what it refers to.
  std::shared_ptr<const DisplayList> list() const;
  // The fonts may be opened from files in these directories only (a path the sender gives that is not in one is not opened).
  void AllowFontDirectories(std::vector<std::string> directories);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::paint
