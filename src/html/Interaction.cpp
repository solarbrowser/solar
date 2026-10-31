/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <cstdlib>
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
  // (Script reads and writes the tree as it is, not as it was committed.)
  dom::CommittedViewSuspend suspend;
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

bool DispatchLocked(dom::Document* document, const char* type, double x, double y, int button, int buttons, double movementX, double movementY) {
  if (!document || !document->context) return true;
  Context& ctx = *document->context;
  Value arguments[] = {qe::FromUtf8(ctx, type), qe::FromNumber(x), qe::FromNumber(y), qe::FromNumber(button), qe::FromNumber(buttons), qe::FromNumber(movementX), qe::FromNumber(movementY)};
  const Value result = CallHelper(document, "locked", arguments, 7);
  return qe::IsUndefined(result) ? true : qe::ToBoolean(result);
}

bool DispatchKey(dom::Document* document, dom::Element* target, const char* type, const std::string& key, const std::string& code, int keyCode, bool repeat, int modifiers) {
  if (!document || !document->context || !target) return true;
  Context& ctx = *document->context;
  Value arguments[] = {qe::FromObject(target), qe::FromUtf8(ctx, type), qe::FromUtf8(ctx, key), qe::FromUtf8(ctx, code), qe::FromNumber(keyCode), qe::FromBool(repeat), qe::FromNumber(modifiers)};
  const Value result = CallHelper(document, "key", arguments, 7);
  return qe::IsUndefined(result) ? true : qe::ToBoolean(result);
}

void DispatchHover(dom::Document* document, dom::Element* from, dom::Element* to, double x, double y) {
  if (!document || !document->context) return;
  Value arguments[] = {from ? qe::FromObject(from) : qe::Null(), to ? qe::FromObject(to) : qe::Null(), qe::FromNumber(x), qe::FromNumber(y)};
  CallHelper(document, "hover", arguments, 4);
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
  // What the control shows as it was committed: no script is asked, so this can be done away from the thread script runs on.
  const dom::ControlState* state = element ? element->ControlInView() : nullptr;
  if (!state) return false;
  out.kind = state->kind;
  out.type = state->type;
  out.value = state->value;
  out.placeholder = state->placeholder;
  out.label = state->label;
  out.checked = state->checked;
  out.indeterminate = state->indeterminate;
  out.disabled = state->disabled;
  out.readOnly = state->readOnly;
  out.start = state->start;
  out.end = state->end;
  out.rows = state->rows;
  out.top = state->top;
  out.multiple = state->multiple;
  out.fraction = state->fraction;
  out.options.clear();
  for (const dom::ControlState::Option& o : state->options) out.options.push_back({o.label, o.selected, o.disabled, o.group, o.indented});
  return true;
}

void RefreshControls(dom::Document* document) {
  if (!document || !document->context) return;
  Context& ctx = *document->context;
  for (dom::Element* element : dom::TakeDirtyControls()) {
    if (element->nodeDocument != document) {
      if (element->nodeDocument) dom::MarkControlDirty(element);
      continue;
    }
    Value arguments[] = {qe::FromObject(element)};
    const Value meta = CallHelper(document, "formMeta", arguments, 1);
    if (!qe::IsObject(meta)) {
      element->control.reset();
      dom::MarkPending(element, dom::kPendingControl);
      continue;
    }
    const auto text = [&](const char* name) { Value v = qe::Get(ctx, meta, name); return qe::IsString(v) ? qe::ToWtf8(ctx, v) : std::string(); };
    const auto flag = [&](const char* name) { return qe::ToBoolean(qe::Get(ctx, meta, name)); };
    const auto number = [&](const char* name) { Value v = qe::Get(ctx, meta, name); return qe::IsNumber(v) ? static_cast<size_t>(qe::ToNumber(ctx, v)) : size_t(0); };
    auto state = std::make_shared<dom::ControlState>();
    state->kind = text("kind");
    state->type = text("type");
    state->value = text("value");
    state->placeholder = text("placeholder");
    state->label = text("label");
    state->checked = flag("checked");
    state->indeterminate = flag("indeterminate");
    state->disabled = flag("disabled");
    state->readOnly = flag("readonly");
    state->start = number("start");
    state->end = number("end");
    state->rows = std::max<size_t>(1, number("rows"));
    state->top = number("top");
    state->multiple = flag("multiple");
    {
      Value f = qe::Get(ctx, meta, "fraction");
      state->fraction = qe::IsNumber(f) ? qe::ToNumber(ctx, f) : 0;
    }
    {
      // The options come as one text: a record for each, its flags (1 selected, 2 disabled, 4 a group's heading) and its label, set apart by control characters.
      const std::string packed = text("options");
      size_t at = 0;
      while (at < packed.size()) {
        size_t end = packed.find('\x1e', at);
        if (end == std::string::npos) end = packed.size();
        const std::string record = packed.substr(at, end - at);
        const size_t split = record.find('\x1f');
        if (split != std::string::npos) {
          const int flags = std::atoi(record.substr(0, split).c_str());
          state->options.push_back({record.substr(split + 1), (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0});
        }
        at = end + 1;
      }
    }
    element->control = std::move(state);
    dom::MarkPending(element, dom::kPendingControl);
  }
}

bool ChooseOption(dom::Document* document, dom::Element* select, size_t index, int mode) {
  if (!document || !document->context || !select) return false;
  Value arguments[] = {qe::FromObject(select), qe::FromNumber(static_cast<double>(index)), qe::FromNumber(mode)};
  const Value result = CallHelper(document, "choose", arguments, 3);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

void SetListTop(dom::Document* document, dom::Element* select, size_t top) {
  if (!document || !document->context || !select) return;
  Value arguments[] = {qe::FromObject(select), qe::FromNumber(static_cast<double>(top))};
  CallHelper(document, "listTop", arguments, 2);
}

bool FocusNext(dom::Document* document, bool backwards) {
  if (!document || !document->context) return false;
  Value arguments[] = {qe::FromBool(backwards)};
  const Value result = CallHelper(document, "tab", arguments, 1);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

bool SetRange(dom::Document* document, dom::Element* target, double fraction, bool final) {
  if (!document || !document->context || !target) return false;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(fraction), qe::FromBool(final)};
  const Value result = CallHelper(document, "range", arguments, 3);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

bool StepControl(dom::Document* document, dom::Element* target, int direction) {
  if (!document || !document->context || !target) return false;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(direction)};
  const Value result = CallHelper(document, "step", arguments, 2);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

bool MoveInRadioGroup(dom::Document* document, dom::Element* target, int direction) {
  if (!document || !document->context || !target) return false;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(direction)};
  const Value result = CallHelper(document, "radio", arguments, 2);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

bool HistoryStep(dom::Document* document, int delta) {
  if (!document || !document->context) return false;
  Value arguments[] = {qe::FromNumber(delta)};
  const Value result = CallHelper(document, "historyGo", arguments, 1);
  return !qe::IsUndefined(result) && qe::ToBoolean(result);
}

void SelectText(dom::Document* document, dom::Node* anchor, uint32_t anchorOffset, dom::Node* focus, uint32_t focusOffset) {
  if (!document || !document->context || !anchor || !focus) return;
  Value arguments[] = {qe::FromObject(anchor), qe::FromNumber(anchorOffset), qe::FromObject(focus), qe::FromNumber(focusOffset)};
  CallHelper(document, "selectText", arguments, 4);
}

void SelectContents(dom::Document* document, dom::Element* element) {
  if (!document || !document->context || !element) return;
  Value arguments[] = {qe::FromObject(element)};
  CallHelper(document, "selectBlock", arguments, 1);
}

void ClearSelection(dom::Document* document) {
  if (!document || !document->context) return;
  CallHelper(document, "clearSelection", nullptr, 0);
}

std::string SelectedText(dom::Document* document) {
  if (!document || !document->context) return "";
  const Value result = CallHelper(document, "selectedText", nullptr, 0);
  return qe::IsString(result) ? qe::ToWtf8(*document->context, result) : std::string();
}

bool DispatchWheel(dom::Document* document, dom::Element* target, double x, double y, double dx, double dy) {
  if (!document || !document->context || !target) return true;
  Value arguments[] = {qe::FromObject(target), qe::FromNumber(x), qe::FromNumber(y), qe::FromNumber(dx), qe::FromNumber(dy)};
  const Value result = CallHelper(document, "wheel", arguments, 5);
  return qe::IsUndefined(result) ? true : qe::ToBoolean(result);
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
