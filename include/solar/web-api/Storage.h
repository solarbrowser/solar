/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where what a page stores (IndexedDB, the Cache API, a service worker's registrations) is kept: files, by origin. The page's script keeps them through this and does not know where they
// are: on the disk of this process (the default, for the tools and tests), or asked of the browser, which is the only one that has the disk (the page's process is shut in).
namespace solar::webapi {

enum class StorageArea : uint8_t { IndexedDb, Cache, Kv };

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;
  // The file `id` of the origin in the area, its first `maxBytes` bytes (all of it for 0). Nothing where there is none.
  virtual std::optional<std::string> Read(StorageArea area, const std::string& origin, const std::string& id, size_t maxBytes = 0) = 0;
  // Writes the file whole (what was there is replaced all at once, or stays). False where it could not be.
  virtual bool Write(StorageArea area, const std::string& origin, const std::string& id, std::string_view bytes) = 0;
  virtual bool Remove(StorageArea area, const std::string& origin, const std::string& id) = 0;
  // The ids of the files of the origin in the area.
  virtual std::vector<std::string> List(StorageArea area, const std::string& origin) = 0;
};

// The storage this process uses: the disk's, unless another is set (not owned; set before anything is stored).
StorageBackend& CurrentStorage();
void SetStorage(StorageBackend* backend);

// The names of the files: what names an id by the text it stands for (a database's name, a cache's), and whether an id is one (so that no name reaches out of the directory it is in).
std::string StorageId(StorageArea area, const std::string& name);
bool ValidStorageId(StorageArea area, const std::string& id);

// The files on a disk: `base` is the directory of IndexedDB (the other areas are beside it, as "base-cache" and "base-kv"), a directory for each origin in each.
class DiskStorage final : public StorageBackend {
 public:
  explicit DiskStorage(std::string base, uint64_t quota = kMostBytesPerOrigin) : base_(std::move(base)), quota_(quota) {}
  std::optional<std::string> Read(StorageArea area, const std::string& origin, const std::string& id, size_t maxBytes = 0) override;
  bool Write(StorageArea area, const std::string& origin, const std::string& id, std::string_view bytes) override;
  bool Remove(StorageArea area, const std::string& origin, const std::string& id) override;
  std::vector<std::string> List(StorageArea area, const std::string& origin) override;

  // What an origin may keep in an area, and the largest file.
  static constexpr uint64_t kMostBytesPerOrigin = uint64_t{1} << 30;
  static constexpr size_t kLargestFile = size_t{60} << 20;

 private:
  std::string Directory(StorageArea area, const std::string& origin) const;
  std::string base_;
  uint64_t quota_;
};

// Where IndexedDB's files are (SOLAR_IDB_DIR names it; otherwise under the user's data directory); empty where there is nowhere.
std::string StorageDirectory();

}  // namespace solar::webapi
