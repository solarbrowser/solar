// The natives under Web Animations (Animation, KeyframeEffect, CSS animations and transitions), which is written in script over them.
#include "solar/css/Animation.h"
#include "solar/css/Cssom.h"
#include "solar/css/Shorthands.h"
#include "solar/css/Style.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Value;

namespace {

std::string StringArg(Context& ctx, qe::Args args, size_t index) { return index < args.size() ? qe::ToWtf8(ctx, args[index]) : std::string(); }

Value Optional(Context& ctx, const std::optional<std::string>& text) { return text ? qe::FromWtf8(ctx, *text) : qe::Null(); }

// __solarAnimInterpolate(property, from, to, progress): the value between, or null if the two do not combine.
Value Interpolate(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 4) return qe::Null();
  return Optional(ctx, InterpolateValues(StringArg(ctx, args, 0), StringArg(ctx, args, 1), StringArg(ctx, args, 2), qe::ToNumber(ctx, args[3])));
}

// __solarAnimAdd(property, a, b): a added to b, or null.
Value Add(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Null();
  return Optional(ctx, AddValues(StringArg(ctx, args, 0), StringArg(ctx, args, 1), StringArg(ctx, args, 2)));
}

// __solarAnimScale(property, value, factor): the value that many times, or null.
Value Scale(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Null();
  return Optional(ctx, ScaleValue(StringArg(ctx, args, 0), StringArg(ctx, args, 1), qe::ToNumber(ctx, args[2])));
}

// __solarAnimInterpolable(property): whether the values of the property are combined and not only switched.
Value Interpolable(Context& ctx, Value, qe::Args args, Value) { return qe::FromBool(IsInterpolableProperty(StringArg(ctx, args, 0))); }

// __solarAnimSet(element, pseudo, records): the values animations give; a record is "property \x1f value", records apart by \x1e.
Value Set(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = args.size() > 0 ? DOMObject::Cast<dom::Element>(args[0]) : nullptr;
  if (!element) return qe::Undefined();
  const std::string text = StringArg(ctx, args, 2);
  AnimatedValues values;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\x1e', at);
    if (end == std::string::npos) end = text.size();
    const size_t split = text.find('\x1f', at);
    if (split != std::string::npos && split < end) values[text.substr(at, split - at)] = text.substr(split + 1, end - split - 1);
    at = end + 1;
  }
  SetAnimatedValues(element, StringArg(ctx, args, 1), std::move(values));
  return qe::Undefined();
}

// __solarAnimComputed(element, pseudo, property): the computed value without what animations give.
Value Computed(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = args.size() > 0 ? DOMObject::Cast<dom::Element>(args[0]) : nullptr;
  if (!element || args.size() < 3) return qe::Null();
  const std::string property = StringArg(ctx, args, 2);
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition || IsShorthand(*definition)) return qe::Null();
  NoStyleFlush noFlush;
  SuppressAnimatedValues(true);
  const std::string value = ComputedValue(ctx, element, property, StringArg(ctx, args, 1));
  SuppressAnimatedValues(false);
  return qe::FromWtf8(ctx, value);
}

// __solarAnimCompute(element, pseudo, property, text): the computed value the property would have for the element if it were declared as
// the text (em and var() worked out against the element), without what animations give.
Value Compute(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = args.size() > 0 ? DOMObject::Cast<dom::Element>(args[0]) : nullptr;
  if (!element || args.size() < 4) return qe::Null();
  const std::string pseudo = StringArg(ctx, args, 1), property = StringArg(ctx, args, 2);
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition || IsShorthand(*definition)) return qe::Null();
  NoStyleFlush noFlush;
  const auto saved = element->animatedValues.find(pseudo) == element->animatedValues.end() ? AnimatedValues() : element->animatedValues[pseudo];
  SetAnimatedValues(element, pseudo, {{property, StringArg(ctx, args, 3)}});
  const std::string value = ComputedValue(ctx, element, property, pseudo);
  SetAnimatedValues(element, pseudo, saved);
  return qe::FromWtf8(ctx, value);
}

// __solarAuthorVersion(): changes when what is declared, or the tree, does.
Value AuthorVersion(Context&, Value, qe::Args, Value) { return qe::FromNumber(static_cast<double>(AuthorStyleVersion() % 9007199254740991ULL)); }

// __solarAnimOnFlush(function): the function to run before a computed style is read when what is declared has changed.
Value OnFlush(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (!document || args.empty()) return qe::Undefined();
  document->styleFlush = qe::IsObject(args[0]) ? args[0].as_object() : nullptr;
  return qe::Undefined();
}

// __solarAnimExpand(property, text): "longhand \x1f value" records (apart by \x1e) for a shorthand declared with the text; null if it is not
// one or the text is not its value. A longhand gives itself.
Value Expand(Context& ctx, Value, qe::Args args, Value) {
  const std::string name = StringArg(ctx, args, 0);
  const PropertyDefinition* definition = FindProperty(name);
  if (!definition) return qe::Null();
  if (!IsShorthand(*definition)) return qe::FromWtf8(ctx, name + "\x1f" + StringArg(ctx, args, 1));
  std::vector<Longhand> longhands;
  if (!ExpandDeclaration(name, StringArg(ctx, args, 1), longhands)) return qe::Null();
  std::string out;
  for (const Longhand& l : longhands) out += l.name + "\x1f" + l.value + "\x1e";
  return qe::FromWtf8(ctx, out);
}

// __solarAnimPropertyKind(property): 0 not a property, 1 a longhand, 2 a shorthand.
Value Kind(Context& ctx, Value, qe::Args args, Value) {
  const PropertyDefinition* definition = FindProperty(StringArg(ctx, args, 0));
  return qe::FromInt32(!definition ? 0 : IsShorthand(*definition) ? 2 : 1);
}

}  // namespace

void InstallAnimationNatives(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarAnimInterpolate", Interpolate, 4);
  qe::DefineGlobalFunction(ctx, "__solarAnimAdd", Add, 3);
  qe::DefineGlobalFunction(ctx, "__solarAnimScale", Scale, 3);
  qe::DefineGlobalFunction(ctx, "__solarAnimInterpolable", Interpolable, 1);
  qe::DefineGlobalFunction(ctx, "__solarAnimSet", Set, 3);
  qe::DefineGlobalFunction(ctx, "__solarAnimComputed", Computed, 3);
  qe::DefineGlobalFunction(ctx, "__solarAnimExpand", Expand, 2);
  qe::DefineGlobalFunction(ctx, "__solarAnimCompute", Compute, 4);
  qe::DefineGlobalFunction(ctx, "__solarAnimOnFlush", OnFlush, 1);
  qe::DefineGlobalFunction(ctx, "__solarAuthorVersion", AuthorVersion, 0);
  qe::DefineGlobalFunction(ctx, "__solarAnimPropertyKind", Kind, 1);
}

}  // namespace solar::css
