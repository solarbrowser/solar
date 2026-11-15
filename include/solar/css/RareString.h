/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace solar::css {

// A string that is empty nearly always: 8 bytes when it is, and a string made when it is not (a declaration has three of them it hardly ever uses, and a page has a hundred and forty
// thousand declarations).
class RareString {
 public:
  RareString() = default;
  RareString(const std::string& text) { if (!text.empty()) text_ = std::make_unique<std::string>(text); }
  RareString(std::string&& text) { if (!text.empty()) text_ = std::make_unique<std::string>(std::move(text)); }
  RareString(const char* text) { if (text && *text) text_ = std::make_unique<std::string>(text); }
  RareString(const RareString& other) { if (other.text_) text_ = std::make_unique<std::string>(*other.text_); }
  RareString(RareString&&) noexcept = default;
  RareString& operator=(const RareString& other) {
    if (this != &other) text_ = other.text_ ? std::make_unique<std::string>(*other.text_) : nullptr;
    return *this;
  }
  RareString& operator=(RareString&&) noexcept = default;
  const std::string& str() const {
    static const std::string none;
    return text_ ? *text_ : none;
  }
  operator const std::string&() const { return str(); }
  operator std::string_view() const { return str(); }
  bool empty() const { return !text_ || text_->empty(); }
  size_t size() const { return str().size(); }
  const char* c_str() const { return str().c_str(); }
  friend std::string operator+(const std::string& a, const RareString& b) { return a + b.str(); }
  friend std::string operator+(const RareString& a, const std::string& b) { return a.str() + b; }
  friend std::string operator+(const char* a, const RareString& b) { return a + b.str(); }
  friend std::string operator+(const RareString& a, const char* b) { return a.str() + b; }
  friend std::string operator+(const RareString& a, const RareString& b) { return a.str() + b.str(); }
  friend bool operator!=(const RareString& a, const char* b) { return a.str() != b; }
  friend bool operator!=(const RareString& a, const std::string& b) { return a.str() != b; }
  template <class... A> size_t find(A&&... a) const { return str().find(std::forward<A>(a)...); }
  friend bool operator==(const RareString& a, const RareString& b) { return a.str() == b.str(); }
  friend bool operator==(const RareString& a, const std::string& b) { return a.str() == b; }
  friend bool operator==(const RareString& a, const char* b) { return a.str() == b; }

 private:
  std::unique_ptr<std::string> text_;
};

// A list of strings that is empty nearly always (the languages of a :lang()): 8 bytes when it is.
class RareStrings {
 public:
  RareStrings() = default;
  RareStrings(const RareStrings& other) { if (other.list_) list_ = std::make_unique<std::vector<std::string>>(*other.list_); }
  RareStrings(RareStrings&&) noexcept = default;
  RareStrings& operator=(const RareStrings& other) {
    if (this != &other) list_ = other.list_ ? std::make_unique<std::vector<std::string>>(*other.list_) : nullptr;
    return *this;
  }
  RareStrings& operator=(RareStrings&&) noexcept = default;
  RareStrings& operator=(std::vector<std::string> list) {
    list_ = list.empty() ? nullptr : std::make_unique<std::vector<std::string>>(std::move(list));
    return *this;
  }
  bool empty() const { return !list_ || list_->empty(); }
  size_t size() const { return list_ ? list_->size() : 0; }
  const std::string& operator[](size_t i) const { return (*list_)[i]; }
  std::string& operator[](size_t i) { return (*list_)[i]; }
  void push_back(std::string text) {
    if (!list_) list_ = std::make_unique<std::vector<std::string>>();
    list_->push_back(std::move(text));
  }
  std::vector<std::string>::const_iterator begin() const { static const std::vector<std::string> none; return list_ ? list_->begin() : none.begin(); }
  std::vector<std::string>::const_iterator end() const { static const std::vector<std::string> none; return list_ ? list_->end() : none.end(); }
  operator const std::vector<std::string>&() const { static const std::vector<std::string> none; return list_ ? *list_ : none; }

 private:
  std::unique_ptr<std::vector<std::string>> list_;
};

}  // namespace solar::css
