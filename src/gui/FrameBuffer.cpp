/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/FrameBuffer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace solar::gui {

SharedMemory::~SharedMemory() { Reset(); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept : fd_(std::exchange(other.fd_, -1)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this != &other) {
    Reset();
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void SharedMemory::Reset() {
  if (data_) ::munmap(data_, size_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
}

bool SharedMemory::Create(size_t size) {
  Reset();
  if (size == 0) return false;
  const int fd = ::memfd_create("solar-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return false;
  // (Sealed: the memory cannot be made smaller by whoever else has it, which would fault the one reading it.)
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0 || ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    ::close(fd);
    return false;
  }
  void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  data_ = static_cast<uint8_t*>(mapped);
  size_ = size;
  return true;
}

bool SharedMemory::Adopt(int fd, size_t size, bool writable) {
  Reset();
  // (The memory has to be as large as it was said to be: a smaller one would be read past its end.)
  struct stat info;
  const int seals = fd >= 0 ? ::fcntl(fd, F_GET_SEALS) : -1;
  const int needed = F_SEAL_SHRINK | F_SEAL_GROW;
  if (size == 0 || seals < 0 || (seals & needed) != needed || ::fstat(fd, &info) != 0 || static_cast<size_t>(info.st_size) < size) {
    ::close(fd);
    return false;
  }
  void* mapped = ::mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  data_ = static_cast<uint8_t*>(mapped);
  size_ = size;
  return true;
}

}  // namespace solar::gui
