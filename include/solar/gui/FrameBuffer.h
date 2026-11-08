/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>

// Memory two processes share (a memfd that is sent as a descriptor): the page draws into it and the window reads it.
namespace solar::gui {

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory();
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;

  // Makes `size` bytes of memory that can be written here (the descriptor can be sent; the other end can map it for reading only if it asks for that).
  bool Create(size_t size);
  // Maps the memory of a descriptor taken over (it is closed here). Refused where the memory is not sealed against being made smaller or larger (whoever has it could then fault the reader),
  // or is smaller than `size`.
  bool Adopt(int fd, size_t size, bool writable);
  // The descriptor, for sending (still owned here).
  int fd() const { return fd_; }
  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  void Reset();

 private:
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace solar::gui
