#include "solar/css/Logical.h"

#include <unordered_map>
#include <unordered_set>

namespace solar::css {

namespace {

struct Sides {
  const char* blockStart;
  const char* blockEnd;
  const char* inlineStart;
  const char* inlineEnd;
};

Sides SidesFor(const WritingContext& context) {
  const bool rtl = context.direction == "rtl";
  const std::string& mode = context.mode;
  if (mode == "vertical-rl" || mode == "sideways-rl") return {"right", "left", rtl ? "bottom" : "top", rtl ? "top" : "bottom"};
  if (mode == "vertical-lr") return {"left", "right", rtl ? "bottom" : "top", rtl ? "top" : "bottom"};
  if (mode == "sideways-lr") return {"left", "right", rtl ? "top" : "bottom", rtl ? "bottom" : "top"};
  return {"top", "bottom", rtl ? "right" : "left", rtl ? "left" : "right"};
}

bool Vertical(const WritingContext& context) { return context.mode != "horizontal-tb"; }

enum class Kind { Edge, Size, Overflow, Radius };

struct Info {
  Kind kind;
  std::string before;  // of the physical name: what comes before the side, the dimension or the axis letter
  std::string after;   // and after
  bool block = false;  // the block axis (else the inline axis)
  bool end = false;    // the end side (else the start); for a radius, the inline side
  bool blockEnd = false;  // for a radius, the block side
};

const std::unordered_map<std::string, Info>& Table() {
  static const std::unordered_map<std::string, Info> table = [] {
    std::unordered_map<std::string, Info> map;
    const auto edge = [&](const std::string& beforeSide, const std::string& afterSide) {
      for (const bool block : {true, false}) {
        for (const bool end : {false, true}) {
          const std::string name = beforeSide + (block ? "block" : "inline") + (end ? "-end" : "-start") + afterSide;
          map[name] = {Kind::Edge, beforeSide, afterSide, block, end, false};
        }
      }
    };
    edge("margin-", "");
    edge("padding-", "");
    edge("scroll-margin-", "");
    edge("scroll-padding-", "");
    edge("inset-", "");
    for (const char* suffix : {"-width", "-style", "-color"}) edge("border-", suffix);
    for (const bool block : {true, false}) {
      const std::string axis = block ? "block" : "inline";
      for (const char* prefix : {"", "min-", "max-"}) map[std::string(prefix) + axis + "-size"] = {Kind::Size, prefix, "", block, false, false};
      map["contain-intrinsic-" + axis + "-size"] = {Kind::Size, "contain-intrinsic-", "", block, false, false};
      map["overflow-" + axis] = {Kind::Overflow, "overflow-", "", block, false, false};
      map["overscroll-behavior-" + axis] = {Kind::Overflow, "overscroll-behavior-", "", block, false, false};
    }
    for (const bool blockEnd : {false, true}) {
      for (const bool inlineEnd : {false, true}) {
        map[std::string("border-") + (blockEnd ? "end" : "start") + (inlineEnd ? "-end" : "-start") + "-radius"] = {Kind::Radius, "", "", true, inlineEnd, blockEnd};
      }
    }
    return map;
  }();
  return table;
}

const std::unordered_set<std::string>& PhysicalNames() {
  static const std::unordered_set<std::string> names = [] {
    std::unordered_set<std::string> set;
    for (const char* side : {"top", "right", "bottom", "left"}) {
      const std::string s = side;
      set.insert(s);
      set.insert("margin-" + s);
      set.insert("padding-" + s);
      set.insert("scroll-margin-" + s);
      set.insert("scroll-padding-" + s);
      for (const char* suffix : {"-width", "-style", "-color"}) set.insert("border-" + s + suffix);
    }
    for (const char* dim : {"width", "height"}) {
      const std::string d = dim;
      set.insert(d);
      set.insert("min-" + d);
      set.insert("max-" + d);
      set.insert("contain-intrinsic-" + d);
    }
    set.insert("overflow-x");
    set.insert("overflow-y");
    set.insert("overscroll-behavior-x");
    set.insert("overscroll-behavior-y");
    for (const char* corner : {"top-left", "top-right", "bottom-left", "bottom-right"}) set.insert(std::string("border-") + corner + "-radius");
    return set;
  }();
  return names;
}

}  // namespace

bool IsLogicalProperty(const std::string& name) { return Table().count(name) > 0; }

bool HasLogicalCounterparts(const std::string& physical) { return PhysicalNames().count(physical) > 0; }

std::string PhysicalOf(const std::string& logical, const WritingContext& context) {
  const auto found = Table().find(logical);
  if (found == Table().end()) return "";
  const Info& info = found->second;
  const Sides sides = SidesFor(context);
  switch (info.kind) {
    case Kind::Edge: return info.before + (info.block ? (info.end ? sides.blockEnd : sides.blockStart) : (info.end ? sides.inlineEnd : sides.inlineStart)) + info.after;
    case Kind::Size: {
      // the inline axis is the width in a horizontal writing mode
      const bool width = info.block == Vertical(context);
      return info.before + (width ? "width" : "height") + info.after;
    }
    case Kind::Overflow: {
      const bool x = info.block == Vertical(context);
      return info.before + (x ? "x" : "y");
    }
    case Kind::Radius: {
      const std::string block = info.blockEnd ? sides.blockEnd : sides.blockStart;
      const std::string inlineSide = info.end ? sides.inlineEnd : sides.inlineStart;
      const bool blockIsVertical = block == "top" || block == "bottom";
      return "border-" + (blockIsVertical ? block + "-" + inlineSide : inlineSide + "-" + block) + "-radius";
    }
  }
  return "";
}

std::vector<std::string> LogicalsOf(const std::string& physical, const WritingContext& context) {
  std::vector<std::string> out;
  for (const auto& [name, info] : Table()) {
    (void)info;
    if (PhysicalOf(name, context) == physical) out.push_back(name);
  }
  return out;
}

}  // namespace solar::css
