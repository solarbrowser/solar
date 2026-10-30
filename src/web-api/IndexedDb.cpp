/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/IndexedDb.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;
namespace fs = std::filesystem;

namespace {

std::string HexOf(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

// What names an origin as a directory: its hash, and what of the origin can be read in a name.
std::string OriginDirectory(const std::string& origin) {
  std::string readable;
  for (char c : origin) {
    if (readable.size() >= 40) break;
    readable += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
  }
  return HexOf(origin) + "-" + readable;
}

fs::path FileOf(const std::string& origin, const std::string& name) {
  return fs::path(IndexedDbDirectory()) / OriginDirectory(origin) / (HexOf(name) + ".idb");
}

bool ReadAll(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

// __solarIdbRead(origin, name): the text of a database as it was written, or undefined.
Value Read(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  std::string text;
  if (!ReadAll(FileOf(qe::ToWtf8(ctx, args[0]), qe::ToWtf8(ctx, args[1])), text)) return qe::Undefined();
  return qe::FromWtf8(ctx, text);
}

// __solarIdbWrite(origin, name, text): true if it was written.
Value Write(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::FromBool(false);
  const fs::path path = FileOf(qe::ToWtf8(ctx, args[0]), qe::ToWtf8(ctx, args[1]));
  const std::string text = qe::ToWtf8(ctx, args[2]);
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) return qe::FromBool(false);
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return qe::FromBool(false);
    out << text;
    if (!out) return qe::FromBool(false);
  }
  fs::rename(temporary, path, error);
  return qe::FromBool(!error);
}

Value Delete(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromBool(false);
  std::error_code error;
  fs::remove(FileOf(qe::ToWtf8(ctx, args[0]), qe::ToWtf8(ctx, args[1])), error);
  return qe::FromBool(!error);
}

// __solarIdbList(origin): the first line of each file of the origin (a header: the name and the version), joined by newlines.
Value List(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::FromUtf8(ctx, "");
  const fs::path directory = fs::path(IndexedDbDirectory()) / OriginDirectory(qe::ToWtf8(ctx, args[0]));
  std::string out;
  std::error_code error;
  for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    if (it->path().extension() != ".idb") continue;
    std::ifstream in(it->path(), std::ios::binary);
    std::string line;
    if (in && std::getline(in, line)) out += line + "\n";
  }
  return qe::FromWtf8(ctx, out);
}

}  // namespace

std::string IndexedDbDirectory() {
  if (const char* given = std::getenv("SOLAR_IDB_DIR")) return given;
  fs::path base;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) base = fs::path(home) / "Library" / "Application Support";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) base = xdg;
  else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".local" / "share";
#endif
  if (base.empty()) return "";
  return (base / "solar" / "indexeddb").string();
}

void InstallIndexedDbNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  if (IndexedDbDirectory().empty()) return;
  qe::DefineGlobalFunction(ctx, "__solarIdbRead", Read, 2);
  qe::DefineGlobalFunction(ctx, "__solarIdbWrite", Write, 3);
  qe::DefineGlobalFunction(ctx, "__solarIdbDelete", Delete, 2);
  qe::DefineGlobalFunction(ctx, "__solarIdbList", List, 1);
}

}  // namespace solar::webapi
