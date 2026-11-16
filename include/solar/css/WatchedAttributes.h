/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

// The names of the attributes that anything of style reads: those a selector looks at ([data-x], :has([data-x]), where ever it came from: a style sheet, querySelector), and, as a
// whole, any attr() of a value. A custom attribute (data-*, aria-*) that is none of these has no effect on the style or the layout of anything, and writing one is not told to them
// (see dom::AttributeIsInert). Header only: the parsers that note the names are used without the DOM.
namespace solar::css {

class WatchedAttributes {
 public:
  static void Watch(std::string_view name) {
    if (!IsCustom(name)) return;
    Instance().Add(name);
  }
  // An attr() was seen: any name may be read from here on.
  static void WatchAll() {
    WatchedAttributes& self = Instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    self.all_ = true;
  }
  // The name (as written) is one that no selector and no attr() reads.
  static bool IsUnwatchedCustom(std::string_view name) {
    if (!IsCustom(name)) return false;
    return Instance().Check(name);
  }

 private:
  static bool IsCustom(std::string_view name) {
    auto starts = [&](std::string_view prefix) {
      if (name.size() < prefix.size()) return false;
      for (size_t i = 0; i < prefix.size(); ++i) if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) return false;
      return true;
    };
    return starts("data-") || starts("aria-");
  }
  static WatchedAttributes& Instance() {
    static WatchedAttributes instance;
    return instance;
  }
  static std::string Lowered(std::string_view name) {
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }
  void Add(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    names_.insert(Lowered(name));
  }
  bool Check(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !all_ && names_.find(Lowered(name)) == names_.end();
  }
  std::mutex mutex_;
  std::unordered_set<std::string> names_;
  bool all_ = false;
};

}  // namespace solar::css
