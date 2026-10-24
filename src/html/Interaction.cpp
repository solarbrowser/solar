#include <string>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/Frames.h"

// What the program does to a page when the user acts on it: a press or a release of the mouse, a click, text typed into a control. Each
// is a call into the window's script (JsForms.cpp), which makes the event, fires it and does what the element does when no script stops it.
namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

Value CallHelper(dom::Document* document, const char* name, Value* arguments, size_t count) {
  if (!document || !document->context) return qe::Undefined();
  Context& ctx = *document->context;
  Object* holder = dom::RealmHolder(ctx);
  if (!holder) return qe::Undefined();
  Value function = qe::Get(ctx, qe::FromObject(holder), name);
  if (!qe::IsCallable(function)) return qe::Undefined();
  Value result = qe::Call(ctx, function, qe::Undefined(), qe::Args(arguments, count));
  if (qe::HasException(ctx)) {
    ctx.clear_exception();
    return qe::Undefined();
  }
  return result;
}

}  // namespace

bool DispatchPointer(dom::Document* document, dom::Element* target, const char* type, double x, double y, int button, int buttons) {
  if (!document || !document->context || !target) return true;
  Context& ctx = *document->context;
  Value arguments[] = {qe::FromObject(target), qe::FromUtf8(ctx, type), qe::FromNumber(x), qe::FromNumber(y), qe::FromNumber(button), qe::FromNumber(buttons)};
  const Value result = CallHelper(document, "pointer", arguments, 6);
  return qe::IsUndefined(result) ? true : qe::ToBoolean(result);
}

bool DispatchClick(dom::Document* document, dom::Element* target, double x, double y, int button) {
  if (!document || !document->context || !target) return true;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(x), qe::FromNumber(y), qe::FromNumber(button)};
  const Value result = CallHelper(document, "click", arguments, 4);
  return qe::IsUndefined(result) ? true : qe::ToBoolean(result);
}

bool TypeInto(dom::Document* document, dom::Element* target, const std::string& text, const char* kind) {
  if (!document || !document->context || !target) return false;
  Context& ctx = *document->context;
  Value arguments[] = {qe::FromObject(target), qe::FromUtf8(ctx, text), qe::FromUtf8(ctx, kind)};
  const Value result = CallHelper(document, "input", arguments, 3);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

bool ReadControl(dom::Element* element, ControlView& out) {
  dom::Document* document = element ? element->nodeDocument : nullptr;
  if (!document || !document->context) return false;
  Context& ctx = *document->context;
  Value arguments[] = {qe::FromObject(element)};
  const Value meta = CallHelper(document, "formMeta", arguments, 1);
  if (!qe::IsObject(meta)) return false;
  const auto text = [&](const char* name) { Value v = qe::Get(ctx, meta, name); return qe::IsString(v) ? qe::ToWtf8(ctx, v) : std::string(); };
  const auto flag = [&](const char* name) { return qe::ToBoolean(qe::Get(ctx, meta, name)); };
  const auto number = [&](const char* name) { Value v = qe::Get(ctx, meta, name); return qe::IsNumber(v) ? static_cast<size_t>(qe::ToNumber(ctx, v)) : size_t(0); };
  out.kind = text("kind");
  out.type = text("type");
  out.value = text("value");
  out.placeholder = text("placeholder");
  out.label = text("label");
  out.checked = flag("checked");
  out.indeterminate = flag("indeterminate");
  out.disabled = flag("disabled");
  out.readOnly = flag("readonly");
  out.start = number("start");
  out.end = number("end");
  return true;
}

void SetCaret(dom::Document* document, dom::Element* target, size_t start, size_t end) {
  if (!document || !document->context || !target) return;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(static_cast<double>(start)), qe::FromNumber(static_cast<double>(end))};
  CallHelper(document, "caret", arguments, 3);
}

void FireChange(dom::Document* document, dom::Element* target) {
  if (!document || !document->context || !target) return;
  Value arguments[] = {qe::FromObject(target)};
  CallHelper(document, "change", arguments, 1);
}

}  // namespace solar::html
