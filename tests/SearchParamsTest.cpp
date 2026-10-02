#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "solar/url/UrlObject.h"
#include "solar/url/UrlSearchParams.h"

namespace {

using solar::url::UrlObject;
using solar::url::UrlSearchParams;
using Pairs = std::vector<UrlSearchParams::Pair>;

int total = 0;
int failed = 0;

template <typename T>
void Expect(const char* what, const T& got, const T& want) {
  ++total;
  if (got == want) return;
  ++failed;
  std::printf("FAIL %s\n", what);
}

std::string Show(const Pairs& pairs) {
  std::string out;
  for (const auto& pair : pairs) out += "[" + pair.first + "," + pair.second + "]";
  return out;
}

void ExpectPairs(const std::string& input, const Pairs& want, bool sort = false) {
  UrlSearchParams params(input);
  if (sort) params.Sort();
  Expect(((sort ? "sort " : "parse ") + input).c_str(), Show(params.List()), Show(want));
}

// The cases below come from WPT's urlencoded-parser, urlsearchparams-sort,
// -stringifier, -delete, -set and -get tests.
void Parsing() {
  ExpectPairs("test", {{"test", ""}});
  ExpectPairs("\xEF\xBB\xBF" "test=\xEF\xBB\xBF", {{"\xEF\xBB\xBF" "test", "\xEF\xBB\xBF"}});
  ExpectPairs("%EF%BB%BFtest=%EF%BB%BF", {{"\xEF\xBB\xBF" "test", "\xEF\xBB\xBF"}});
  ExpectPairs("%EF%BF%BF=%EF%BF%BF", {{"\xEF\xBF\xBF", "\xEF\xBF\xBF"}});
  ExpectPairs("%FE%FF", {{"\xEF\xBF\xBD\xEF\xBF\xBD", ""}});
  ExpectPairs("%FF%FE", {{"\xEF\xBF\xBD\xEF\xBF\xBD", ""}});
  ExpectPairs("\xE2\x80\xA0&\xE2\x80\xA0=x", {{"\xE2\x80\xA0", ""}, {"\xE2\x80\xA0", "x"}});
  ExpectPairs("%C2", {{"\xEF\xBF\xBD", ""}});
  ExpectPairs("%C2x", {{"\xEF\xBF\xBD" "x", ""}});
  ExpectPairs("", {});
  ExpectPairs("a", {{"a", ""}});
  ExpectPairs("a=b", {{"a", "b"}});
  ExpectPairs("a=", {{"a", ""}});
  ExpectPairs("=b", {{"", "b"}});
  ExpectPairs("&", {});
  ExpectPairs("&a", {{"a", ""}});
  ExpectPairs("a&", {{"a", ""}});
  ExpectPairs("a&a", {{"a", ""}, {"a", ""}});
  ExpectPairs("a&b&c", {{"a", ""}, {"b", ""}, {"c", ""}});
  ExpectPairs("&&&a=b&&&&c=d&", {{"a", "b"}, {"c", "d"}});
  ExpectPairs("a=a&a=b&a=c", {{"a", "a"}, {"a", "b"}, {"a", "c"}});
  ExpectPairs("a==a", {{"a", "=a"}});
  ExpectPairs("a=a+b+c+d", {{"a", "a b c d"}});
  ExpectPairs("%=a", {{"%", "a"}});
  ExpectPairs("%a=a", {{"%a", "a"}});
  ExpectPairs("%a_=a", {{"%a_", "a"}});
  ExpectPairs("%61=a", {{"a", "a"}});
  ExpectPairs("%61+%4d%4D=", {{"a MM", ""}});
  ExpectPairs("id=0&value=%", {{"id", "0"}, {"value", "%"}});
  ExpectPairs("b=%2sf%2a", {{"b", "%2sf*"}});
  ExpectPairs("b=%2%2af%2a", {{"b", "%2*f*"}});
  ExpectPairs("b=%%2a", {{"b", "%*"}});
  ExpectPairs("?a=b", {{"a", "b"}});
}

void Sorting() {
  ExpectPairs("z=b&a=b&z=a&a=a", {{"a", "b"}, {"a", "a"}, {"z", "b"}, {"z", "a"}}, true);
  ExpectPairs("\xEF\xBF\xBD=x&\xEF\xBF\xBC&\xEF\xBF\xBD=a",
              {{"\xEF\xBF\xBC", ""}, {"\xEF\xBF\xBD", "x"}, {"\xEF\xBF\xBD", "a"}}, true);
  // U+1F308 is above U+FB03 as a code point but sorts first as a surrogate pair.
  ExpectPairs("\xEF\xAC\x83&\xF0\x9F\x8C\x88", {{"\xF0\x9F\x8C\x88", ""}, {"\xEF\xAC\x83", ""}}, true);
  ExpectPairs("\xC3\xA9&e\xEF\xBF\xBD&e\xCC\x81",
              {{"e\xCC\x81", ""}, {"e\xEF\xBF\xBD", ""}, {"\xC3\xA9", ""}}, true);
  ExpectPairs("bbb&bb&aaa&aa=x&aa=y", {{"aa", "x"}, {"aa", "y"}, {"aaa", ""}, {"bb", ""}, {"bbb", ""}}, true);
  ExpectPairs("z=z&=f&=t&=x", {{"", "f"}, {"", "t"}, {"", "x"}, {"z", "z"}}, true);
}

void Serializing() {
  UrlSearchParams params;
  params.Append("a", "b c");
  Expect("space value", params.ToString(), std::string("a=b+c"));
  params.Delete("a");
  params.Append("a b", "c");
  Expect("space name", params.ToString(), std::string("a+b=c"));

  UrlSearchParams empties;
  empties.Append("a", "");
  empties.Append("a", "");
  empties.Append("", "b");
  empties.Append("", "");
  Expect("empties", empties.ToString(), std::string("a=&a=&=b&="));

  UrlSearchParams specials;
  specials.Append("a", "b+c");
  specials.Append("=", "&");
  specials.Append("*-._", "b%c");
  Expect("specials", specials.ToString(), std::string("a=b%2Bc&%3D=%26&*-._=b%25c"));

  Expect("percent roundtrip", UrlSearchParams("id=0&value=%").ToString(), std::string("id=0&value=%25"));
  Expect("tilde", UrlSearchParams("a=b ~").ToString(), std::string("a=b+%7E"));
}

void Mutating() {
  UrlSearchParams params("a=a&b=b&a=a&c=c");
  params.Delete("a");
  Expect("delete all by name", params.ToString(), std::string("b=b&c=c"));

  UrlSearchParams pair;
  pair.Append("a", "b");
  pair.Append("a", "c");
  pair.Append("a", "d");
  pair.Delete("a", "c");
  Expect("delete by name and value", pair.ToString(), std::string("a=b&a=d"));

  UrlSearchParams set("a=b&c=d&a=e");
  set.Set("a", "B");
  Expect("set replaces first and drops rest", set.ToString(), std::string("a=B&c=d"));
  set.Set("e", "f");
  Expect("set appends", set.ToString(), std::string("a=B&c=d&e=f"));

  UrlSearchParams lookup("a=1&a=2&a=3");
  Expect("get first", lookup.Get("a").value_or("<none>"), std::string("1"));
  Expect("get missing", lookup.Get("b").has_value(), false);
  Expect("getAll", lookup.GetAll("a"), std::vector<std::string>{"1", "2", "3"});
  Expect("has", lookup.Has("a", "2"), true);
  Expect("has wrong value", lookup.Has("a", "9"), false);
  Expect("size", lookup.Size(), size_t{3});
}

void LinkedToUrl() {
  auto url = UrlObject::Create("http://example.com/?param1&param2");
  url->SearchParams().Delete("param1");
  url->SearchParams().Delete("param2");
  Expect("delete all drops the ?", url->Href(), std::string("http://example.com/"));
  Expect("delete all search", url->Search(), std::string(""));

  auto question = UrlObject::Create("http://example.com/?");
  question->SearchParams().Delete("param1");
  Expect("deleting from empty query drops the ?", question->Href(), std::string("http://example.com/"));

  auto sorted = UrlObject::Create("https://example.com/?a=b ~");
  Expect("query keeps its own encoding", sorted->Href(), std::string("https://example.com/?a=b%20~"));
  sorted->SearchParams().Sort();
  Expect("sort re-serializes as urlencoded", sorted->Href(), std::string("https://example.com/?a=b+%7E"));

  auto reading = UrlObject::Create("https://example.com/?a=~&b=%7E");
  Expect("get a", reading->SearchParams().Get("a").value_or(""), std::string("~"));
  Expect("get b", reading->SearchParams().Get("b").value_or(""), std::string("~"));

  auto opaque = UrlObject::Create("data:space    ?test#test");
  opaque->SearchParams().Delete("test");
  Expect("opaque path keeps trailing space encoding", opaque->Href(), std::string("data:space   %20#test"));

  auto search = UrlObject::Create("https://example.com/?a=b");
  search->SetSearch("?c=d&e=f");
  Expect("search setter syncs list", search->SearchParams().Get("e").value_or(""), std::string("f"));
  search->SetSearch("");
  Expect("empty search clears list", search->SearchParams().Size(), size_t{0});
  Expect("empty search clears query", search->Href(), std::string("https://example.com/"));

  auto href = UrlObject::Create("https://example.com/?a=b");
  href->SetHref("https://other.example/?x=y");
  Expect("href setter syncs list", href->SearchParams().Get("x").value_or(""), std::string("y"));
  Expect("href setter failure keeps url", href->SetHref("not a url"), false);
  Expect("href unchanged after failure", href->Href(), std::string("https://other.example/?x=y"));

  Expect("bad base fails", UrlObject::Create("/x", "not a url") == nullptr, true);
  Expect("relative with base", UrlObject::Create("/x", "https://a.example/y")->Href(), std::string("https://a.example/x"));
}

}  // namespace

int main() {
  Parsing();
  Sorting();
  Serializing();
  Mutating();
  LinkedToUrl();
  std::printf("searchparams: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
