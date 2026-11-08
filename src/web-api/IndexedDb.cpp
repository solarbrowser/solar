/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/IndexedDb.h"

#include <cstdint>
#include <optional>
#include <string>

#include "solar/web-api/Storage.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

constexpr StorageArea kArea = StorageArea::IndexedDb;

// __solarIdbRead(origin, name): the text of a database as it was written, or undefined.
Value Read(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  const std::optional<std::string> text = CurrentStorage().Read(kArea, qe::ToWtf8(ctx, args[0]), StorageId(kArea, qe::ToWtf8(ctx, args[1])));
  if (!text) return qe::Undefined();
  return qe::FromWtf8(ctx, *text);
}

// __solarIdbWrite(origin, name, text): true if it was written.
Value Write(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::FromBool(false);
  return qe::FromBool(CurrentStorage().Write(kArea, qe::ToWtf8(ctx, args[0]), StorageId(kArea, qe::ToWtf8(ctx, args[1])), qe::ToWtf8(ctx, args[2])));
}

Value Delete(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromBool(false);
  return qe::FromBool(CurrentStorage().Remove(kArea, qe::ToWtf8(ctx, args[0]), StorageId(kArea, qe::ToWtf8(ctx, args[1]))));
}

// __solarIdbList(origin): the first line of each file of the origin (a header: the name and the version), joined by newlines.
Value List(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::FromUtf8(ctx, "");
  const std::string origin = qe::ToWtf8(ctx, args[0]);
  std::string out;
  for (const std::string& id : CurrentStorage().List(kArea, origin)) {
    // (The header is all that is asked for of a file: it can be large.)
    const std::optional<std::string> head = CurrentStorage().Read(kArea, origin, id, 4096);
    if (!head) continue;
    const size_t end = head->find('\n');
    out += head->substr(0, end) + "\n";
  }
  return qe::FromWtf8(ctx, out);
}

}  // namespace

std::string IndexedDbDirectory() { return StorageDirectory(); }

void InstallIndexedDbNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  if (IndexedDbDirectory().empty()) return;
  qe::DefineGlobalFunction(ctx, "__solarIdbRead", Read, 2);
  qe::DefineGlobalFunction(ctx, "__solarIdbWrite", Write, 3);
  qe::DefineGlobalFunction(ctx, "__solarIdbDelete", Delete, 2);
  qe::DefineGlobalFunction(ctx, "__solarIdbList", List, 1);
}

}  // namespace solar::webapi
