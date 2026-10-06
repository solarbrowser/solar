#include "solar/web/Messaging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "solar/web/DomBindingsInternal.h"
#include "solar/web/ErrorReporting.h"
#include "solar/web/FetchBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_portPrototypeKey;
char g_channelPrototypeKey;

struct JsMessagePort;

// What the two ports of a channel share with each other: the messages that have come for this one, and whether the
// other end is still there. It outlives the cell, and moves from one cell to another when the port is transferred.
struct PortCore {
  std::weak_ptr<PortCore> peer;
  std::deque<std::unique_ptr<qe::SerializedData>> queue;
  bool started = false;
  bool closed = false;
  bool deliveryQueued = false;
  JsMessagePort* owner = nullptr;
  qe::Realm* realm = nullptr;
};

struct JsMessagePort : JsEventTarget {
  using Parent = JsEventTarget;
  std::shared_ptr<PortCore> core;  // empty once the port has been transferred away

  ~JsMessagePort() {
    if (core && core->owner == this) core->owner = nullptr;
  }
  void Visit(Quanta::Visitor& visitor) { JsEventTarget::Visit(visitor); }
};

struct JsMessageChannel : Quanta::DOMObject {
  JsMessagePort* port1 = nullptr;
  JsMessagePort* port2 = nullptr;
  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(port1);
    visitor.Mark(port2);
  }
};

// The cores of ports in transit: a transferred port is its core, until the message that carries it is received.
std::mutex g_transitMutex;
std::map<uint64_t, std::shared_ptr<PortCore>>& Transit() {
  static std::map<uint64_t, std::shared_ptr<PortCore>> transit;
  return transit;
}
uint64_t g_nextTransit = 1;

// The ports made while a message is deserialized, by the number they travelled under, rooted: a message does not have
// to refer to a port that came with it.
struct ReceivedPorts {
  std::vector<uint64_t> ids;
  qe::ValueList ports;
};
ReceivedPorts& Received() {
  static ReceivedPorts received;
  return received;
}
void ResetReceived() { Received() = ReceivedPorts(); }

Object* PortPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_portPrototypeKey)); }

JsMessagePort* ThisPort(Context& ctx, const Value& t) {
  JsMessagePort* port = DOMObject::Cast<JsMessagePort>(t);
  if (!port) qe::ThrowTypeError(ctx, "Illegal invocation");
  return port;
}

// The ports of a realm that have a reason to be kept: started, and with the other end still there. They are in a set
// on the realm's holder, which is what keeps a port nobody refers to alive while a message can still come for it.
void KeepAlive(Context& ctx, JsMessagePort* port, bool keep) {
  Value global = qe::FromObject(ctx.get_global_object());
  static const char kName[] = "__solarLivePorts";
  Value set = qe::Get(ctx, global, kName);
  if (!qe::IsObject(set)) {
    if (!keep) return;
    set = qe::Construct(ctx, qe::Get(ctx, global, "Set"));
    qe::Descriptor d;
    d.value = set;
    d.has_value = true;
    d.has_writable = d.writable = false;
    d.has_enumerable = d.enumerable = false;
    d.has_configurable = d.configurable = true;
    qe::DefineProperty(ctx, global, kName, d);
  }
  Value method = qe::Get(ctx, set, keep ? "add" : "delete");
  Value argument = qe::FromObject(port);
  qe::Call(ctx, method, set, qe::Args(&argument, 1));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

void ScheduleDelivery(const std::shared_ptr<PortCore>& core);

// One message: the event the port gets for it, or messageerror if it cannot be made.
void DeliverOne(const std::shared_ptr<PortCore>& core) {
  core->deliveryQueued = false;
  if (!core->owner || !core->realm || !core->started || core->closed || core->queue.empty()) return;
  JsMessagePort* port = core->owner;
  Context& ctx = core->realm->GetContext();
  std::unique_ptr<qe::SerializedData> data = std::move(core->queue.front());
  core->queue.pop_front();
  ResetReceived();
  Value message = qe::Deserialize(ctx, *data);
  qe::ValueList ports = PortsOf(ctx, *data);
  const bool failed = qe::HasException(ctx);
  if (failed) ctx.clear_exception();
  Value global = qe::FromObject(ctx.get_global_object());
  Value init = qe::NewObject(ctx);
  if (!failed) {
    qe::Set(ctx, init, "data", message);
    Value portArray = qe::NewArray(ctx, {});
    for (size_t i = 0; i < ports.size(); ++i) qe::ArrayPush(ctx, portArray, ports[i]);
    qe::Set(ctx, init, "ports", portArray);
  }
  Value arguments[] = {qe::FromUtf8(ctx, failed ? "messageerror" : "message"), init};
  Value event = qe::Construct(ctx, qe::Get(ctx, global, "MessageEvent"), qe::Args(arguments, 2));
  if (qe::HasException(ctx)) {
    ctx.clear_exception();
  } else if (JsEvent* jsEvent = DOMObject::Cast<JsEvent>(event)) {
    jsEvent->isTrusted = true;
    DispatchOn(ctx, port, jsEvent);
    if (qe::HasException(ctx)) ReportException(ctx);
  }
  ScheduleDelivery(core);
}

void ScheduleDelivery(const std::shared_ptr<PortCore>& core) {
  if (core->deliveryQueued || !core->owner || !core->realm || !core->started || core->closed || core->queue.empty()) return;
  core->deliveryQueued = true;
  core->realm->EnqueueTask("message", [core] { DeliverOne(core); });
}

void StartPort(Context& ctx, JsMessagePort* port) {
  if (!port->core || port->core->started) return;
  port->core->started = true;
  KeepAlive(ctx, port, true);
  ScheduleDelivery(port->core);
}

// ---- The objects ----

JsMessagePort* NewPort(Context& ctx, std::shared_ptr<PortCore> core) {
  JsMessagePort* port = Heap::Allocate<JsMessagePort>();
  port->initialize_prototype(PortPrototype(ctx));
  port->core = std::move(core);
  port->core->owner = port;
  port->core->realm = qe::Realm::FromContext(ctx);
  return port;
}

// The sequence<object> of a transfer list, from the second argument of postMessage: the list or { transfer }.
bool ReadTransfer(Context& ctx, qe::Args args, size_t index, std::vector<Value>& out, qe::ValueList& keep, const char* member) {
  if (args.size() <= index || qe::IsUndefined(args[index])) return true;
  Value list = args[index];
  if (!qe::IsObject(list)) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'MessagePort': parameter 2 is not of type 'StructuredSerializeOptions'.");
    return false;
  }
  // Not a sequence (no iterator): the options dictionary, which has `transfer`.
  Value iterator = qe::GetIteratorMethod(ctx, list);
  if (qe::HasException(ctx)) return false;
  if (qe::IsUndefined(iterator)) {
    list = qe::Get(ctx, list, "transfer");
    if (qe::HasException(ctx)) return false;
    if (qe::IsUndefined(list)) return true;
    if (!qe::IsObject(list)) {
      qe::ThrowTypeError(ctx, "The provided value cannot be converted to a sequence.");
      return false;
    }
  }
  Value array = qe::Call(ctx, qe::Get(ctx, qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "Array"), "from"), qe::Undefined(), qe::Args(&list, 1));
  if (qe::HasException(ctx)) return false;
  const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, array, "length"));
  for (uint32_t i = 0; i < length; ++i) {
    Value item = qe::GetIndex(ctx, array, i);
    if (qe::HasException(ctx)) return false;
    if (!qe::IsObject(item)) {
      qe::ThrowTypeError(ctx, "The transfer list must contain objects.");
      return false;
    }
    keep.Append(item);
    out.push_back(item);
  }
  return true;
}

Value PostMessage(Context& ctx, Value t, qe::Args args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'postMessage' on 'MessagePort': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  qe::SerializeOptions options;
  qe::ValueList keep;
  if (!ReadTransfer(ctx, args, 1, options.transfer, keep, "postMessage")) return qe::Undefined();
  // A port cannot be sent over itself.
  for (const Value& item : options.transfer) {
    if (DOMObject::Cast<JsMessagePort>(item) == self) {
      ThrowDomException(ctx, "Cannot transfer a MessagePort over itself.", "DataCloneError");
      return qe::Undefined();
    }
  }
  auto data = std::make_unique<qe::SerializedData>();
  if (!qe::Serialize(ctx, args[0], options, *data)) return qe::Undefined();
  if (!self->core || self->core->closed) return qe::Undefined();
  const std::shared_ptr<PortCore> peer = self->core->peer.lock();
  if (!peer || peer->closed) return qe::Undefined();
  peer->queue.push_back(std::move(data));
  ScheduleDelivery(peer);
  return qe::Undefined();
}

Value Start(Context& ctx, Value t, qe::Args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  if (self) StartPort(ctx, self);
  return qe::Undefined();
}

Value Close(Context& ctx, Value t, qe::Args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  if (!self || !self->core) return qe::Undefined();
  const std::shared_ptr<PortCore> core = self->core;
  core->closed = true;
  core->queue.clear();
  if (auto peer = core->peer.lock()) peer->peer.reset();
  core->peer.reset();
  KeepAlive(ctx, self, false);
  return qe::Undefined();
}

Value GetOnMessage(Context& ctx, Value t, qe::Args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  return self ? GetEventHandler(self, "message") : qe::Undefined();
}

Value SetOnMessage(Context& ctx, Value t, qe::Args args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  if (!self) return qe::Undefined();
  SetEventHandler(self, "message", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  // Setting the handler is what starts a port.
  StartPort(ctx, self);
  return qe::Undefined();
}

Value GetOnMessageError(Context& ctx, Value t, qe::Args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  return self ? GetEventHandler(self, "messageerror") : qe::Undefined();
}

Value SetOnMessageError(Context& ctx, Value t, qe::Args args, Value) {
  JsMessagePort* self = ThisPort(ctx, t);
  if (self) SetEventHandler(self, "messageerror", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}

Value ConstructPort(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value ConstructChannel(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'MessageChannel': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget, &g_channelPrototypeKey);
  if (qe::HasException(ctx)) return qe::Undefined();
  auto core1 = std::make_shared<PortCore>();
  auto core2 = std::make_shared<PortCore>();
  core1->peer = core2;
  core2->peer = core1;
  JsMessageChannel* channel = Heap::Allocate<JsMessageChannel>();
  channel->initialize_prototype(prototype);
  channel->port1 = NewPort(ctx, core1);
  channel->port2 = NewPort(ctx, core2);
  return qe::FromObject(channel);
}

Value GetPort1(Context& ctx, Value t, qe::Args, Value) {
  JsMessageChannel* self = DOMObject::Cast<JsMessageChannel>(t);
  if (!self) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return qe::FromObject(self->port1);
}

Value GetPort2(Context& ctx, Value t, qe::Args, Value) {
  JsMessageChannel* self = DOMObject::Cast<JsMessageChannel>(t);
  if (!self) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return qe::FromObject(self->port2);
}

// ---- What is cloned and what is transferred ----

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void PutU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void PutString(std::vector<uint8_t>& out, const std::string& text) {
  PutU32(out, static_cast<uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

struct Reader {
  const std::vector<uint8_t>& bytes;
  size_t pos = 0;
  uint32_t U32() {
    uint32_t value = 0;
    for (int i = 0; i < 4 && pos < bytes.size(); ++i) value |= static_cast<uint32_t>(bytes[pos++]) << (8 * i);
    return value;
  }
  uint64_t U64() {
    uint64_t value = 0;
    for (int i = 0; i < 8 && pos < bytes.size(); ++i) value |= static_cast<uint64_t>(bytes[pos++]) << (8 * i);
    return value;
  }
  std::string String() {
    const uint32_t length = U32();
    std::string text(reinterpret_cast<const char*>(bytes.data()) + std::min<size_t>(pos, bytes.size()), std::min<size_t>(length, bytes.size() - std::min(pos, bytes.size())));
    pos += text.size();
    return text;
  }
};

// The port a transferred one came to be, in the realm of `ctx`, made from its core in transit.
Value MakeReceivedPort(Context& ctx, uint64_t id) {
  std::shared_ptr<PortCore> core;
  {
    std::lock_guard<std::mutex> lock(g_transitMutex);
    const auto found = Transit().find(id);
    if (found != Transit().end()) {
      core = found->second;
      Transit().erase(found);
    }
  }
  if (!core) {
    ThrowDomException(ctx, "The MessagePort has already been received.", "DataCloneError");
    return qe::Undefined();
  }
  JsMessagePort* port = NewPort(ctx, core);
  if (core->started) KeepAlive(ctx, port, true);
  ScheduleDelivery(core);
  Received().ids.push_back(id);
  Received().ports.Append(qe::FromObject(port));
  return qe::FromObject(port);
}

// ---- BroadcastChannel ----

struct JsBroadcastChannel : JsEventTarget {
  using Parent = JsEventTarget;
  std::string name, origin;
  qe::Realm* realm = nullptr;
  bool closed = false;
  ~JsBroadcastChannel();
  void Visit(Quanta::Visitor& visitor) { JsEventTarget::Visit(visitor); }
};

char g_broadcastPrototypeKey;

std::vector<JsBroadcastChannel*>& Channels() {
  static std::vector<JsBroadcastChannel*> channels;
  return channels;
}

JsBroadcastChannel::~JsBroadcastChannel() { Channels().erase(std::remove(Channels().begin(), Channels().end(), this), Channels().end()); }

JsBroadcastChannel* ThisChannel(Context& ctx, const Value& t) {
  JsBroadcastChannel* channel = DOMObject::Cast<JsBroadcastChannel>(t);
  if (!channel) qe::ThrowTypeError(ctx, "Illegal invocation");
  return channel;
}

std::string OriginOfGlobal(Context& ctx) {
  Value location = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "location");
  std::string origin = "null";
  if (qe::IsObject(location)) {
    Value value = qe::Get(ctx, location, "origin");
    if (!qe::HasException(ctx) && value.is_string()) origin = qe::ToWtf8(ctx, value);
    if (qe::HasException(ctx)) ctx.clear_exception();
  }
  return origin;
}

Value ConstructBroadcast(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'BroadcastChannel': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to construct 'BroadcastChannel': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget, &g_broadcastPrototypeKey);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsBroadcastChannel* channel = Heap::Allocate<JsBroadcastChannel>();
  channel->initialize_prototype(prototype);
  channel->name = name;
  channel->origin = OriginOfGlobal(ctx);
  channel->realm = qe::Realm::FromContext(ctx);
  Channels().push_back(channel);
  return qe::FromObject(channel);
}

Value BroadcastName(Context& ctx, Value t, qe::Args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  return self ? qe::FromWtf8(ctx, self->name) : qe::Undefined();
}

Value BroadcastPostMessage(Context& ctx, Value t, qe::Args args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'postMessage' on 'BroadcastChannel': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  if (self->closed) {
    ThrowDomException(ctx, "BroadcastChannel is closed.", "InvalidStateError");
    return qe::Undefined();
  }
  auto data = std::make_shared<qe::SerializedData>();
  if (!qe::Serialize(ctx, args[0], {}, *data)) return qe::Undefined();
  for (JsBroadcastChannel* other : Channels()) {
    if (other == self || other->closed || other->name != self->name || other->origin != self->origin || !other->realm) continue;
    auto keep = std::make_shared<qe::Persistent>(other->realm->GetContext(), qe::FromObject(other));
    qe::Realm* realm = other->realm;
    const std::string origin = self->origin;
    realm->EnqueueTask("broadcast", [keep, realm, data, origin] {
      JsBroadcastChannel* channel = DOMObject::Cast<JsBroadcastChannel>(keep->Get());
      if (!channel || channel->closed) return;
      Context& tctx = realm->GetContext();
      qe::SerializedData copy = *data;
      Value message = qe::Deserialize(tctx, copy);
      const bool failed = qe::HasException(tctx);
      if (failed) tctx.clear_exception();
      Value init = qe::NewObject(tctx);
      if (!failed) {
        qe::Set(tctx, init, "data", message);
        qe::Set(tctx, init, "origin", qe::FromWtf8(tctx, origin));
      }
      Value arguments[] = {qe::FromUtf8(tctx, failed ? "messageerror" : "message"), init};
      Value event = qe::Construct(tctx, qe::Get(tctx, qe::FromObject(tctx.get_global_object()), "MessageEvent"), qe::Args(arguments, 2));
      if (qe::HasException(tctx)) {
        tctx.clear_exception();
        return;
      }
      if (JsEvent* jsEvent = DOMObject::Cast<JsEvent>(event)) {
        jsEvent->isTrusted = true;
        DispatchOn(tctx, channel, jsEvent);
        if (qe::HasException(tctx)) ReportException(tctx);
      }
    });
  }
  return qe::Undefined();
}

Value BroadcastClose(Context& ctx, Value t, qe::Args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  if (self) {
    self->closed = true;
  }
  return qe::Undefined();
}

Value GetBroadcastOnMessage(Context& ctx, Value t, qe::Args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  return self ? GetEventHandler(self, "message") : qe::Undefined();
}
Value SetBroadcastOnMessage(Context& ctx, Value t, qe::Args args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  if (self) SetEventHandler(self, "message", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}
Value GetBroadcastOnMessageError(Context& ctx, Value t, qe::Args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  return self ? GetEventHandler(self, "messageerror") : qe::Undefined();
}
Value SetBroadcastOnMessageError(Context& ctx, Value t, qe::Args args, Value) {
  JsBroadcastChannel* self = ThisChannel(ctx, t);
  if (self) SetEventHandler(self, "messageerror", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}

}  // namespace

// The ports that came with a message, in the order of its transfer list: those the message refers to were made as it
// was read, and the others, which the engine leaves alone, are made now.
qe::ValueList PortsOf(Context& ctx, qe::SerializedData& data) {
  qe::ValueList ports;
  for (qe::SerializedData::Transfer& transfer : data.transfers) {
    if (transfer.kind != qe::SerializedData::Transfer::Kind::Host) continue;
    const qe::SerializedData::HostTransfer& host = data.host_transfers[transfer.index];
    if (host.tag != "MessagePort") continue;
    Reader reader{host.bytes};
    const uint64_t id = reader.U64();
    Value port;
    bool found = false;
    for (size_t i = 0; i < Received().ids.size(); ++i) {
      if (Received().ids[i] == id) {
        port = Received().ports[i];
        found = true;
      }
    }
    if (!found && !transfer.consumed) {
      port = MakeReceivedPort(ctx, id);
      transfer.consumed = true;
      if (qe::HasException(ctx)) continue;
    }
    if (qe::IsObject(port)) ports.Append(port);
  }
  ResetReceived();
  return ports;
}

qe::SerializationHooks MakeSerializationHooks() {
  qe::SerializationHooks hooks;
  hooks.serialize = [](qe::Realm*, const Value& object, const qe::SerializationMode& mode, qe::HostObjectData& out, std::string& error) {
    if (JsMessagePort* port = DOMObject::Cast<JsMessagePort>(object)) {
      if (!mode.transferring) {
        error = "A MessagePort can only be transferred.";
        return false;
      }
      if (!port->core) {
        error = "The MessagePort has been transferred already.";
        return false;
      }
      // The port goes, as its core; the cell is left with nothing.
      std::shared_ptr<PortCore> core = std::move(port->core);
      port->core.reset();
      core->owner = nullptr;
      core->deliveryQueued = false;
      uint64_t id;
      {
        std::lock_guard<std::mutex> lock(g_transitMutex);
        id = g_nextTransit++;
        Transit()[id] = core;
      }
      out.tag = "MessagePort";
      PutU64(out.bytes, id);
      return true;
    }
    if (mode.transferring) {
      error = "The object cannot be transferred.";
      return false;
    }
    if (JsFile* file = DOMObject::Cast<JsFile>(object)) {
      out.tag = "File";
      PutString(out.bytes, file->type);
      PutString(out.bytes, file->name);
      PutU64(out.bytes, static_cast<uint64_t>(file->lastModified));
      const std::string_view bytes = file->Bytes();
      out.bytes.insert(out.bytes.end(), bytes.begin(), bytes.end());
      return true;
    }
    if (JsBlob* blob = DOMObject::Cast<JsBlob>(object)) {
      out.tag = "Blob";
      PutString(out.bytes, blob->type);
      const std::string_view bytes = blob->Bytes();
      out.bytes.insert(out.bytes.end(), bytes.begin(), bytes.end());
      return true;
    }
    if (JsDomException* exception = DOMObject::Cast<JsDomException>(object)) {
      out.tag = "DOMException";
      PutString(out.bytes, exception->name);
      PutString(out.bytes, exception->message);
      return true;
    }
    error = "The object could not be cloned.";
    return false;
  };
  hooks.isTransferable = [](qe::Realm*, const Value& object) { return DOMObject::Cast<JsMessagePort>(object) != nullptr; };
  hooks.deserialize = [](qe::Realm* realm, const qe::HostObjectData& data, bool) -> Value {
    Context& ctx = realm->GetContext();
    Reader reader{data.bytes};
    if (data.tag == "MessagePort") return MakeReceivedPort(ctx, reader.U64());
    if (data.tag == "Blob") {
      const std::string type = reader.String();
      return qe::FromObject(NewBlob(ctx, std::string(reinterpret_cast<const char*>(data.bytes.data()) + reader.pos, data.bytes.size() - reader.pos), type));
    }
    if (data.tag == "File") {
      const std::string type = reader.String();
      const std::string name = reader.String();
      const int64_t modified = static_cast<int64_t>(reader.U64());
      return qe::FromObject(NewFile(ctx, std::string(reinterpret_cast<const char*>(data.bytes.data()) + reader.pos, data.bytes.size() - reader.pos), name, type, modified));
    }
    if (data.tag == "DOMException") {
      const std::string name = reader.String();
      const std::string message = reader.String();
      return NewDomException(ctx, message, name);
    }
    ThrowDomException(ctx, "The object could not be deserialized.", "DataCloneError");
    return qe::Undefined();
  };
  return hooks;
}

void DefineMessagingClasses(Context& ctx) {
  qe::ClassRef port = qe::DefineClass(ctx, "MessagePort", ConstructPort, 0, EventTargetPrototype(ctx));
  qe::SetRealmData(ctx, &g_portPrototypeKey, port.prototype);
  qe::DefineMethod(port.prototype, "postMessage", PostMessage, 1);
  qe::DefineMethod(port.prototype, "start", Start, 0);
  qe::DefineMethod(port.prototype, "close", Close, 0);
  qe::DefineAccessor(port.prototype, "onmessage", GetOnMessage, SetOnMessage);
  qe::DefineAccessor(port.prototype, "onmessageerror", GetOnMessageError, SetOnMessageError);
  qe::DefineGlobal(ctx, "MessagePort", port.constructor);

  qe::ClassRef broadcast = qe::DefineClass(ctx, "BroadcastChannel", ConstructBroadcast, 1, EventTargetPrototype(ctx));
  qe::SetRealmData(ctx, &g_broadcastPrototypeKey, broadcast.prototype);
  qe::DefineAccessor(broadcast.prototype, "name", BroadcastName, nullptr);
  qe::DefineMethod(broadcast.prototype, "postMessage", BroadcastPostMessage, 1);
  qe::DefineMethod(broadcast.prototype, "close", BroadcastClose, 0);
  qe::DefineAccessor(broadcast.prototype, "onmessage", GetBroadcastOnMessage, SetBroadcastOnMessage);
  qe::DefineAccessor(broadcast.prototype, "onmessageerror", GetBroadcastOnMessageError, SetBroadcastOnMessageError);
  qe::DefineGlobal(ctx, "BroadcastChannel", broadcast.constructor);

  qe::ClassRef channel = qe::DefineClass(ctx, "MessageChannel", ConstructChannel, 0);
  qe::SetRealmData(ctx, &g_channelPrototypeKey, channel.prototype);
  qe::DefineAccessor(channel.prototype, "port1", GetPort1, nullptr);
  qe::DefineAccessor(channel.prototype, "port2", GetPort2, nullptr);
  qe::DefineGlobal(ctx, "MessageChannel", channel.constructor);
}

}  // namespace solar::web
