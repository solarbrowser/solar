// The files of what a page stores: kept by origin, by an id that is no path, bounded, written whole.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "solar/web-api/Storage.h"

using namespace solar::webapi;
namespace fs = std::filesystem;

namespace {
int total = 0, failed = 0;
void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}
}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "solar-storage-test";
  fs::remove_all(root);
  DiskStorage disk((root / "indexeddb").string());
  const std::string a = "https://a.test", b = "https://b.test";
  const std::string id = StorageId(StorageArea::IndexedDb, "db1");

  Check("an id is 16 hex digits and the area's extension", ValidStorageId(StorageArea::IndexedDb, id) && id.size() == 16 + 4 && !ValidStorageId(StorageArea::Cache, id));
  for (const std::string& bad : {"", "../../etc/passwd", "0123456789abcdef", "0123456789abcdef.idb/", "../0123456789abcdef.idb", "0123456789ABCDEF.idb", "0123456789abcdeg.idb", "0123456789abcdef.idb\\x00", "0123456789abcdef.IDB"}) {
    Check("an id that is no id is refused: " + bad.substr(0, 20), !ValidStorageId(StorageArea::IndexedDb, bad));
  }

  Check("nothing is there at first", !disk.Read(StorageArea::IndexedDb, a, id) && disk.List(StorageArea::IndexedDb, a).empty());
  Check("a file is written", disk.Write(StorageArea::IndexedDb, a, id, std::string("one\0two\nthree", 13)));
  const auto back = disk.Read(StorageArea::IndexedDb, a, id);
  Check("and read back as it was, with its bytes", back && *back == std::string("one\0two\nthree", 13));
  Check("a head of it is read", disk.Read(StorageArea::IndexedDb, a, id, 3) == std::optional<std::string>("one"));
  Check("it is listed", disk.List(StorageArea::IndexedDb, a) == std::vector<std::string>{id});
  Check("another origin has none of it", !disk.Read(StorageArea::IndexedDb, b, id) && disk.List(StorageArea::IndexedDb, b).empty());
  Check("another area has none of it", disk.List(StorageArea::Cache, a).empty() && !disk.Read(StorageArea::Kv, a, StorageId(StorageArea::Kv, "db1")));
  Check("a file is replaced whole", disk.Write(StorageArea::IndexedDb, a, id, "second") && disk.Read(StorageArea::IndexedDb, a, id) == std::optional<std::string>("second"));
  Check("no temporary file is left", [&] {
    for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.path().extension() == ".tmp") return false;
    return true;
  }());
  Check("a file that is not an id is not written", !disk.Write(StorageArea::IndexedDb, a, "../../escape.idb", "x") && !fs::exists(root / "escape.idb") && !fs::exists(fs::temp_directory_path() / "escape.idb"));
  Check("nor read", !disk.Read(StorageArea::IndexedDb, a, "../" + id));
  Check("an origin with odd characters is a directory of its own and none above", disk.Write(StorageArea::IndexedDb, "../../x/..", id, "odd") && !fs::exists(root / "x"));
  Check("a file is removed", disk.Remove(StorageArea::IndexedDb, a, id) && !disk.Read(StorageArea::IndexedDb, a, id));
  Check("a file larger than the most is not written", !disk.Write(StorageArea::IndexedDb, a, id, std::string(DiskStorage::kLargestFile + 1, 'x')));

  // The bound for an origin: what it has and what it adds.
  {
    DiskStorage small((root / "small").string(), 1000);
    Check("a file within the quota is written", small.Write(StorageArea::Cache, b, StorageId(StorageArea::Cache, "one"), std::string(600, 'y')));
    Check("one that would take it over is not", !small.Write(StorageArea::Cache, b, StorageId(StorageArea::Cache, "two"), std::string(600, 'y')));
    Check("a file replaced counts as what it replaces", small.Write(StorageArea::Cache, b, StorageId(StorageArea::Cache, "one"), std::string(900, 'z')));
    Check("another origin has a quota of its own", small.Write(StorageArea::Cache, a, StorageId(StorageArea::Cache, "mine"), std::string(900, 'y')));
  }

  // The default storage is the disk's unless another is set.
  {
    struct Mine : StorageBackend {
      int calls = 0;
      std::optional<std::string> Read(StorageArea, const std::string&, const std::string&, size_t) override { ++calls; return "mine"; }
      bool Write(StorageArea, const std::string&, const std::string&, std::string_view) override { ++calls; return true; }
      bool Remove(StorageArea, const std::string&, const std::string&) override { ++calls; return true; }
      std::vector<std::string> List(StorageArea, const std::string&) override { ++calls; return {}; }
    } mine;
    SetStorage(&mine);
    Check("a storage that is set is the one used", CurrentStorage().Read(StorageArea::Kv, a, "x") == std::optional<std::string>("mine") && mine.calls == 1);
    SetStorage(nullptr);
  }
  fs::remove_all(root);
  std::printf("storage: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
