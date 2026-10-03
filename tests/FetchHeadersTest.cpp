#include <cstdio>
#include <string>

#include "solar/net/FetchHeaders.h"

namespace {

using namespace solar::net;
using Status = FetchHeaders::Status;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

std::string Joined(const std::vector<FetchHeaders::Entry>& entries) {
  std::string out;
  for (const auto& [name, value] : entries) out += name + "=" + value + ";";
  return out;
}

}  // namespace

int main() {
  // Names and values.
  Check("a token is a name", IsValidHeaderName("Content-Type") && IsValidHeaderName("x_y.z~!#$%&'*+^`|"));
  Check("anything else is not", !IsValidHeaderName("") && !IsValidHeaderName("a b") && !IsValidHeaderName("a:b") && !IsValidHeaderName("a\n") &&
                                !IsValidHeaderName("\xC3\xA9") && !IsValidHeaderName("a(b)"));
  Check("a value is stripped of HTTP whitespace at the ends only", NormalizeHeaderValue(" \t\r\n a  b \n\t") == "a  b");
  Check("a value without NUL, CR and LF is valid", IsValidHeaderValue("a\tb \x7f\x80") && !IsValidHeaderValue(std::string("a\0b", 3)) &&
                                                    !IsValidHeaderValue("a\nb") && !IsValidHeaderValue("a\rb"));
  Check("a ByteString keeps U+0000 to U+00FF as single bytes", ToByteString("caf\xC3\xA9") == std::string("caf\xE9") && ToByteString("\xC2\x80") == std::string("\x80") &&
                                                              ToByteString("plain") == std::string("plain"));
  Check("and refuses what is above", !ToByteString("\xC4\x80") && !ToByteString("\xE2\x82\xAC") && !ToByteString("\xF0\x9F\x98\x80") && !ToByteString("\xC3"));
  Check("isomorphic decoding is the way back", IsomorphicDecode("caf\xE9") == "caf\xC3\xA9" && IsomorphicDecode(std::string("\x80\xFF")) == "\xC2\x80\xC3\xBF");

  // The list.
  {
    FetchHeaders h;
    Check("append and get", h.Append("Accept", "a") == Status::Ok && h.Get("accept") == "a" && h.Has("ACCEPT") && !h.Has("accepted"));
    h.Append("accept", " b ");
    Check("repeats are joined with a comma and a space, in order", h.Get("Accept") == "a, b");
    Check("a missing header is none", !h.Get("nothing"));
    h.Append("X-Other", "1");
    h.Set("ACCEPT", "only");
    Check("set replaces, and the others of that name go", h.Get("accept") == "only" && h.list().size() == 2 && h.list()[0].second == "only");
    h.Set("New", "n");
    Check("a new name goes at the end", h.list().back().first == "New");
    h.Delete("accept");
    Check("delete removes them all", !h.Has("accept") && h.list().size() == 2);
    Check("a value that is not valid is refused", h.Append("a", "x\ny") == Status::InvalidValue && h.Set("a", std::string("x\0", 2)) == Status::InvalidValue);
  }
  {
    FetchHeaders h;
    h.Append("b", "2");
    h.Append("A", "1");
    h.Append("b", "3");
    h.Append("Set-Cookie", "x=1");
    h.Append("c", "4");
    h.Append("set-cookie", "y=2");
    Check("sorted by lower case name, values combined, Set-Cookie apart",
          Joined(h.SortedAndCombined()) == "a=1;b=2, 3;c=4;set-cookie=x=1;set-cookie=y=2;", Joined(h.SortedAndCombined()));
    Check("and getSetCookie lists them", h.GetSetCookie() == std::vector<std::string>({"x=1", "y=2"}));
    Check("Get joins them like any other", h.Get("set-cookie") == "x=1, y=2");
  }
  {
    FetchHeaders h;
    h.Append("Z", "1");
    h.Append("a", "2");
    h.Append("M", "3");
    Check("byte order, not locale", Joined(h.SortedAndCombined()) == "a=2;m=3;z=1;");
  }

  // Guards.
  {
    FetchHeaders h;
    h.Append("a", "1");
    h.SetGuard(HeadersGuard::Immutable);
    Check("an immutable list refuses every change", h.Append("b", "1") == Status::Immutable && h.Set("a", "2") == Status::Immutable &&
                                                     h.Delete("a") == Status::Immutable && h.Get("a") == "1");
  }
  {
    FetchHeaders h(HeadersGuard::Request);
    for (const char* name : {"Accept-Charset", "accept-encoding", "Access-Control-Request-Headers", "Access-Control-Request-Method", "Connection", "Content-Length",
                             "Cookie", "Cookie2", "Date", "DNT", "Expect", "Host", "Keep-Alive", "Origin", "Referer", "Set-Cookie", "TE", "Trailer",
                             "Transfer-Encoding", "Upgrade", "Via", "Proxy-Authorization", "proxy-x", "Sec-Fetch-Mode", "sec-x"}) {
      h.Append(name, "v");
    }
    Check("a request's forbidden headers are dropped without an error", h.list().empty(), std::to_string(h.list().size()));
    h.Append("Authorization", "x");
    h.Append("X-Custom", "y");
    h.Append("Secure", "z");
    Check("the rest go in", h.list().size() == 3);
    h.Append("X-HTTP-Method-Override", "PUT");
    h.Append("X-HTTP-Method-Override", "get, TRACE");
    h.Append("x-method-override", " connect");
    h.Append("X-HTTP-Method", "track");
    Check("a method override naming a forbidden method is dropped", h.Get("x-http-method-override") == "PUT" && !h.Has("x-method-override") && !h.Has("x-http-method"));
    h.Set("host", "evil");
    h.Delete("cookie");
    Check("set and delete take the same care", !h.Has("host"));
  }
  {
    FetchHeaders h(HeadersGuard::Response);
    h.Append("Set-Cookie", "x=1");
    h.Append("set-cookie2", "x=1");
    h.Append("Cookie", "kept");
    h.Append("Content-Length", "5");
    Check("a response hides Set-Cookie and nothing else", !h.Has("set-cookie") && !h.Has("set-cookie2") && h.Has("cookie") && h.Has("content-length"));
    h.AppendUnchecked("Set-Cookie", "forced");
    Check("unless it is filled without the checks", h.Has("set-cookie"));
  }
  {
    FetchHeaders h(HeadersGuard::RequestNoCors);
    h.Append("Accept", "*/*");
    h.Append("Accept-Language", "en-US, tr;q=0.8");
    h.Append("Content-Language", "tr");
    h.Append("Content-Type", "text/plain;charset=UTF-8");
    Check("a no-cors request keeps the four, with safe values", h.list().size() == 4);
    h.Append("X-Custom", "v");
    h.Append("Authorization", "v");
    h.Append("Range", "bytes=0-1");
    Check("and nothing else", h.list().size() == 4);
    FetchHeaders g(HeadersGuard::RequestNoCors);
    g.Append("Content-Type", "application/json");
    g.Append("Content-Type", "Multipart/Form-Data; boundary=x");
    g.Append("Accept", "a(b)");
    g.Append("Accept-Language", "tr_TR");
    g.Append("Accept", std::string(129, 'a'));
    Check("values that could start a preflight are refused", g.list().size() == 1 && g.list()[0].first == "Content-Type" && g.list()[0].second.starts_with("Multipart"), Joined(g.list()));
    g.Set("content-type", "application/xml");
    g.Delete("x-nothing");
    FetchHeaders d(HeadersGuard::RequestNoCors);
    d.Append("Content-Type", "text/plain");
    d.Delete("Content-Type");
    d.Append("Accept", "*/*");
    d.AppendUnchecked("X-Other", "kept");
    d.Delete("x-other");
    Check("a removal looks at the name only", !d.Has("content-type") && d.Has("accept") && d.Has("x-other"));
    Check("set and delete take the same care", g.Get("content-type")->starts_with("Multipart"));
    FetchHeaders c(HeadersGuard::RequestNoCors);
    c.Append("Accept", "a");
    c.Append("Accept", "b\"");
    Check("what the combined value would be must be safe too", c.Get("accept") == "a");
  }

  std::printf("fetch headers: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
