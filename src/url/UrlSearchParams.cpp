#include "solar/url/UrlSearchParams.h"

#include <algorithm>

#include "solar/url/PercentEncode.h"
#include "solar/url/Utf8.h"

namespace solar::url {

namespace {

std::vector<UrlSearchParams::Pair> ParseUrlencoded(std::string_view input) {
  std::vector<UrlSearchParams::Pair> output;
  size_t start = 0;
  while (start <= input.size()) {
    size_t end = input.find('&', start);
    if (end == std::string_view::npos) end = input.size();
    std::string_view bytes = input.substr(start, end - start);
    start = end + 1;
    if (bytes.empty()) continue;

    size_t equals = bytes.find('=');
    std::string name(bytes.substr(0, equals));
    std::string value = equals == std::string_view::npos ? "" : std::string(bytes.substr(equals + 1));
    std::replace(name.begin(), name.end(), '+', ' ');
    std::replace(value.begin(), value.end(), '+', ' ');
    output.emplace_back(ScrubUtf8(PercentDecode(name)), ScrubUtf8(PercentDecode(value)));
  }
  return output;
}

// Sorting compares UTF-16 code units, which orders supplementary characters before
// U+E000..U+FFFF; comparing UTF-8 bytes would put them after.
std::u16string ToUtf16(std::string_view utf8) {
  std::u16string out;
  for (char32_t c : DecodeUtf8(utf8)) {
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(c));
    }
  }
  return out;
}

}  // namespace

UrlSearchParams::UrlSearchParams(std::string_view init) {
  if (init.starts_with('?')) init.remove_prefix(1);
  list_ = ParseUrlencoded(init);
}

void UrlSearchParams::Append(std::string_view name, std::string_view value) {
  list_.emplace_back(name, value);
  Update();
}

void UrlSearchParams::Delete(std::string_view name, std::optional<std::string_view> value) {
  std::erase_if(list_, [&](const Pair& pair) { return pair.first == name && (!value || pair.second == *value); });
  Update();
}

std::optional<std::string> UrlSearchParams::Get(std::string_view name) const {
  for (const Pair& pair : list_) {
    if (pair.first == name) return pair.second;
  }
  return std::nullopt;
}

std::vector<std::string> UrlSearchParams::GetAll(std::string_view name) const {
  std::vector<std::string> values;
  for (const Pair& pair : list_) {
    if (pair.first == name) values.push_back(pair.second);
  }
  return values;
}

bool UrlSearchParams::Has(std::string_view name, std::optional<std::string_view> value) const {
  return std::any_of(list_.begin(), list_.end(), [&](const Pair& pair) {
    return pair.first == name && (!value || pair.second == *value);
  });
}

void UrlSearchParams::Set(std::string_view name, std::string_view value) {
  auto first = std::find_if(list_.begin(), list_.end(), [&](const Pair& pair) { return pair.first == name; });
  if (first == list_.end()) {
    list_.emplace_back(name, value);
  } else {
    first->second = value;
    list_.erase(std::remove_if(first + 1, list_.end(), [&](const Pair& pair) { return pair.first == name; }),
                list_.end());
  }
  Update();
}

void UrlSearchParams::Sort() {
  std::vector<std::pair<std::u16string, size_t>> keys;
  keys.reserve(list_.size());
  for (size_t i = 0; i < list_.size(); ++i) keys.emplace_back(ToUtf16(list_[i].first), i);
  std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<Pair> sorted;
  sorted.reserve(list_.size());
  for (const auto& key : keys) sorted.push_back(std::move(list_[key.second]));
  list_ = std::move(sorted);
  Update();
}

std::string UrlSearchParams::ToString() const {
  std::string out;
  for (const Pair& pair : list_) {
    if (!out.empty()) out.push_back('&');
    AppendFormEncoded(out, pair.first);
    out.push_back('=');
    AppendFormEncoded(out, pair.second);
  }
  return out;
}

void UrlSearchParams::ReplaceFromQuery(std::string_view query) { list_ = ParseUrlencoded(query); }

void UrlSearchParams::Update() {
  if (!url_) return;
  std::string serialized = ToString();
  if (serialized.empty()) {
    url_->query.reset();
  } else {
    url_->query = std::move(serialized);
  }
}

}  // namespace solar::url
