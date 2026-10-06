#pragma once

#include <vector>

#include "quanta/Embed.h"

// Cross-document messaging and channel messaging (https://html.spec.whatwg.org/multipage/web-messaging.html and
// #channel-messaging): MessageChannel and MessagePort, and the serialization of what the platform has that is
// cloned (Blob, File, DOMException) or transferred (MessagePort) by structuredClone and postMessage.
namespace solar::web {

// The hooks to give an Isolate with SetSerializationHooks.
Quanta::Embed::SerializationHooks MakeSerializationHooks();

// The ports that came with a message just deserialized in `ctx`'s realm, in the order of its transfer list, which the
// MessageEvent has as its ports.
Quanta::Embed::ValueList PortsOf(Quanta::Context& ctx, Quanta::Embed::SerializedData& data);

}  // namespace solar::web
