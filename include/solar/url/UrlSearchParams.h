#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "solar/url/Url.h"

namespace solar::url {

// Names and values are UTF-8 and must already be valid, as the standard's USVStrings are.
class UrlSearchParams {
 public:
  using Pair = std::pair<std::string, std::string>;

  UrlSearchParams() = default;
  // Drops one leading '?', as the standard's constructor does.
  explicit UrlSearchParams(std::string_view init);

  size_t Size() const { return list_.size(); }
  const std::vector<Pair>& List() const { return list_; }

  void Append(std::string_view name, std::string_view value);
  void Delete(std::string_view name, std::optional<std::string_view> value = std::nullopt);
  std::optional<std::string> Get(std::string_view name) const;
  std::vector<std::string> GetAll(std::string_view name) const;
  bool Has(std::string_view name, std::optional<std::string_view> value = std::nullopt) const;
  void Set(std::string_view name, std::string_view value);
  void Sort();
  std::string ToString() const;

 private:
  friend class UrlObject;

  void ReplaceFromQuery(std::string_view query);
  void Update();

  // The URL whose query follows this list; set only by UrlObject.
  Url* url_ = nullptr;
  std::vector<Pair> list_;
};

}  // namespace solar::url
