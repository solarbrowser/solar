/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/UiNatives.h"

#include "solar/gui/Wire.h"

namespace solar::gui {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

net::ipc::Channel* g_link = nullptr;

Value Send(Context& ctx, Value, qe::Args args, Value) {
  if (!g_link || args.empty()) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (text.size() > (size_t{1} << 16)) return qe::Undefined();
  g_link->SendRaw(static_cast<uint8_t>(wire::ToBrowser::UiCommand), text);
  return qe::Undefined();
}

}  // namespace

void InstallUiNatives(qe::Realm& realm, net::ipc::Channel& link) {
  g_link = &link;
  qe::DefineGlobalFunction(realm.GetContext(), "__solarUiSend", Send, 1);
}

}  // namespace solar::gui
