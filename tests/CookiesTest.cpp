#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "solar/net/Cookies.h"
#include "solar/url/Parser.h"
#include "support/Json.h"

namespace {

using namespace std::chrono_literals;
using solar::net::Cookie;
using solar::net::CookieJar;
using solar::net::CookieRequest;
using solar::net::SameSite;
using solar::test::Json;

using Time = std::chrono::system_clock::time_point;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

solar::url::Url U(const std::string& text) { return *solar::url::Parse(text); }

Time At(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  // The same calendar arithmetic the other way round, to keep the test independent of the code.
  const int a = (14 - month) / 12;
  const int y = year + 4800 - a;
  const int m = month + 12 * a - 3;
  const long jdn = day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
  const long days = jdn - 2440588;
  return Time(std::chrono::seconds(days * 86400 + hour * 3600 + minute * 60 + second));
}

std::string FormatDate(Time time) {
  const long total = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
  const long days = total / 86400;
  const long rest = total % 86400;
  static const char* const kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const long z = days + 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const long doe = z - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp = (5 * doy + 2) / 153;
  const long day = doy - (153 * mp + 2) / 5 + 1;
  const long month = mp < 10 ? mp + 3 : mp - 9;
  const long year = yoe + era * 400 + (month <= 2);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s, %02ld %s %04ld %02ld:%02ld:%02ld GMT", kDays[((days % 7) + 7) % 7], day, kMonths[month - 1], year,
                rest / 3600, rest / 60 % 60, rest % 60);
  return buffer;
}

// A jar whose clock the test moves.
struct Clocked {
  Time now = At(2020, 6, 1);
  CookieJar jar{[this] { return now; }};
};

// "a=1; b=2" as the name and value pairs a request would carry.
std::string Header(Clocked& c, const std::string& url, CookieRequest request = {}) { return c.jar.HeaderFor(U(url), request); }

// The data files start with a licence in // comments, which JSON has no place for.
std::optional<Json> LoadWithComments(const std::string& path) {
  std::ifstream file(path);
  if (!file) return std::nullopt;
  std::string text, line;
  while (std::getline(file, line)) {
    if (!line.starts_with("//")) text += line + "\n";
  }
  return solar::test::JsonReader(text).Read();
}

// The project wrote its parser tests before RFC 6265bis let a cookie have no name. It keeps what
// it receives as "foo" or "=foo" as a cookie whose value is foo, and sends it as just "foo".
// These are what the tests become under it, each worked out by hand from the headers.
const std::map<std::string, std::string> kNamelessCookies = {
    {"0004", "=foo;"},          {"0021", "a=b;=x;c=d;"},     {"0023", "=foo;"},       {"0024", "=foo;"},
    {"0025", "=foo;"},          {"0026", "=foo;"},           {"0027", "=bar;"},       {"0028", "=foo;"},
    {"CHROMIUM0009", "=BLAHHH;"}, {"CHROMIUM0010", "=\"BLA\\\"HHH\";"}, {"CHROMIUM0012", "=ABC;"},
    {"MOZILLA0012", "test=\"fubar! = foo;=five;"},          {"MOZILLA0014", "=six;"}, {"MOZILLA0015", "=seven;"},
    {"MOZILLA0016", "=eight;"}, {"MOZILLA0017", "=eight;test=six;"}, {"NAME0017", "=a=bar;"}, {"NAME0023", "=foo;"},
    {"NAME0025", "===a=bar;"},  {"NAME0028", "=a;"},         {"NAME0031", "=\"foo;"}, {"NAME0032", "=\"foo\\\"bar;"},
    {"NAME0033", "=aaa;"},
};

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = std::string(argc > 1 ? argv[1] : "tests/data") + "/cookies";

  // The http-state project's parser tests: Set-Cookie headers in, the cookies sent back out.
  if (auto tests = LoadWithComments(dir + "/parser.json")) {
    int run = 0;
    for (const Json& test : tests->array) {
      const std::string name = test.Find("test")->string;
      if (name.starts_with("DISABLED_")) continue;
      Clocked c;
      c.now = At(2013, 6, 1);
      std::string lowered = name;
      for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const std::string origin = "http://home.example.org:8888";
      const auto setUrl = U(origin + "/cookie-parser?" + lowered);
      for (const Json& received : test.Find("received")->array) c.jar.Store(setUrl, received.string, {});

      std::string target = origin + "/cookie-parser-result?" + lowered;
      if (const Json* to = test.Find("sent-to")) target = to->string.starts_with("http") ? to->string : origin + to->string;
      const auto matching = c.jar.Matching(U(target), {});

      std::string got, want;
      for (const Cookie& cookie : matching) got += cookie.name + "=" + cookie.value + ";";
      for (const Json& sent : test.Find("sent")->array) want += sent.Find("name")->string + "=" + sent.Find("value")->string + ";";
      ++run;
      std::string headers;
      for (const Json& received : test.Find("received")->array) headers += " [" + received.string + "]";
      if (const auto it = kNamelessCookies.find(name); it != kNamelessCookies.end()) want = it->second;
      Check("http-state " + name, got == want, "got <" + got + "> want <" + want + "> for" + headers);
    }
    Check("parser tests were read", run > 150, std::to_string(run));
  } else {
    Check("parser.json opens", false);
  }

  // The dates, from the same project and from Chromium.
  for (const char* file : {"dates-examples.json", "dates-bsd-examples.json"}) {
    auto tests = LoadWithComments(dir + "/" + file);
    Check(std::string(file) + " opens", tests.has_value());
    if (!tests) continue;
    for (const Json& test : tests->array) {
      const std::string input = test.Find("test")->string;
      const Json* expected = test.Find("expected");
      const auto parsed = solar::net::ParseCookieDate(input);
      if (expected->type == Json::Type::Null) {
        Check("date <" + input + "> is refused", !parsed.has_value(), parsed ? FormatDate(*parsed) : "");
      } else {
        Check("date <" + input + ">", parsed && FormatDate(*parsed) == expected->string, parsed ? FormatDate(*parsed) : "no date");
      }
    }
  }


  // ---- Where a cookie goes: its domain ----
  {
    Clocked c;
    c.jar.Store(U("http://www.example.com/"), "a=1", {});
    Check("a cookie without Domain goes to its host only", Header(c, "http://www.example.com/") == "a=1" && Header(c, "http://sub.www.example.com/").empty() &&
                                                           Header(c, "http://example.com/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://www.example.com/"), "a=1; Domain=.Example.COM", {});
    Check("Domain takes in the domain and its subdomains", Header(c, "http://example.com/") == "a=1" && Header(c, "http://www.example.com/") == "a=1" &&
                                                           Header(c, "http://a.b.example.com/") == "a=1");
    Check("but not a lookalike", Header(c, "http://notexample.com/").empty() && Header(c, "http://example.org/").empty() &&
                                 Header(c, "http://example.com.evil.net/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://www.example.com/"), "a=1; Domain=other.com", {});
    c.jar.Store(U("http://www.example.com/"), "b=1; Domain=sub.www.example.com", {});
    Check("a Domain the host is not under is refused", c.jar.size() == 0);
  }
  {
    Clocked c;
    c.jar.Store(U("http://www.example.com/"), "a=1; Domain=com", {});
    c.jar.Store(U("http://www.example.co.uk/"), "b=1; Domain=co.uk", {});
    c.jar.Store(U("http://alice.github.io/"), "c=1; Domain=github.io", {});
    Check("a Domain that is a public suffix is refused", c.jar.size() == 0);
  }
  {
    Clocked c;
    c.jar.Store(U("http://co.uk/"), "a=1; Domain=co.uk", {});
    Check("unless the host is that name, when it is host-only", Header(c, "http://co.uk/") == "a=1" && Header(c, "http://x.co.uk/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://127.0.0.1/"), "a=1; Domain=127.0.0.1", {});
    c.jar.Store(U("http://127.0.0.1/"), "b=1; Domain=0.0.1", {});
    Check("an address is matched whole, never as a suffix", Header(c, "http://127.0.0.1/") == "a=1" && c.jar.size() == 1);
  }
  {
    Clocked c;
    c.jar.Store(U("http://a.example.com/"), "a=1; Domain=example.com; Domain=", {});
    Check("an empty Domain is ignored", Header(c, "http://b.example.com/") == "a=1");
  }

  // ---- Where a cookie goes: its path ----
  {
    Clocked c;
    c.jar.Store(U("http://example.com/a/b/c"), "x=1", {});
    Check("the default path is the request's up to its last slash",
          Header(c, "http://example.com/a/b") == "x=1" && Header(c, "http://example.com/a/b/z") == "x=1" && Header(c, "http://example.com/a/bc").empty() &&
              Header(c, "http://example.com/a").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/x"), "r=1", {});
    c.jar.Store(U("http://example.com/"), "s=1", {});
    Check("at the root it is /", Header(c, "http://example.com/anything") == "r=1; s=1");
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; Path=/foo", {});
    Check("an explicit path", Header(c, "http://example.com/foo") == "a=1" && Header(c, "http://example.com/foo/") == "a=1" && Header(c, "http://example.com/foo/x") == "a=1");
    Check("matches whole segments", Header(c, "http://example.com/foobar").empty() && Header(c, "http://example.com/fo").empty() && Header(c, "http://example.com/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/a/b/"), "x=1; Path=nope", {});
    Check("a Path that does not start with a slash falls back to the default", Header(c, "http://example.com/a/b/") == "x=1");
    c.jar.Store(U("http://example.com/"), "y=1; Path=/p; Path=/q", {});
    Check("the last Path wins", Header(c, "http://example.com/q") == "y=1" && Header(c, "http://example.com/p").empty());
    c.jar.Store(U("http://example.com/"), "z=1; Path=/" + std::string(1100, 'p'), {});
    Check("an attribute value over 1024 bytes is ignored", Header(c, "http://example.com/anything") == "z=1");
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "root=1; Path=/", {});
    c.now += 1s;
    c.jar.Store(U("http://example.com/"), "deep=1; Path=/a/b", {});
    c.jar.Store(U("http://example.com/"), "mid=1; Path=/a", {});
    Check("longer paths first, then the older", Header(c, "http://example.com/a/b/c") == "deep=1; mid=1; root=1");
    c.now += 1s;
    c.jar.Store(U("http://example.com/"), "root=2; Path=/", {});
    c.jar.Store(U("http://example.com/"), "other=1; Path=/", {});
    Check("a replaced cookie keeps its place", Header(c, "http://example.com/a/b/c") == "deep=1; mid=1; root=2; other=1");
  }

  // ---- How long it lasts ----
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "s=1; Max-Age=10", {});
    c.jar.Store(U("http://example.com/"), "session=1", {});
    c.now += 9s;
    Check("until Max-Age is up", Header(c, "http://example.com/") == "s=1; session=1");
    c.now += 2s;
    Check("then gone, and a session cookie stays", Header(c, "http://example.com/") == "session=1" && c.jar.size() == 1);
    c.now += 24h * 1000;
    Check("however long", Header(c, "http://example.com/") == "session=1");
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; Max-Age=10; Expires=Wed, 01 Jan 2200 00:00:00 GMT", {});
    c.now += 20s;
    Check("Max-Age beats Expires", Header(c, "http://example.com/").empty());
    c.jar.Store(U("http://example.com/"), "b=1; Expires=" + FormatDate(c.now + 100s), {});
    c.now += 99s;
    Check("Expires alone", Header(c, "http://example.com/") == "b=1");
    c.now += 2s;
    Check("is a date", Header(c, "http://example.com/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; Max-Age=0", {});
    Check("an already expired cookie is not kept", c.jar.size() == 0);
    c.jar.Store(U("http://example.com/"), "a=1", {});
    c.jar.Store(U("http://example.com/"), "a=1; Max-Age=0", {});
    Check("Max-Age=0 deletes the cookie it names", c.jar.size() == 0);
    c.jar.Store(U("http://example.com/"), "a=1; Path=/x", {});
    c.jar.Store(U("http://example.com/"), "a=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/x", {});
    Check("so does an Expires in the past", c.jar.size() == 0);
    c.jar.Store(U("http://example.com/"), "a=1; Path=/x", {});
    c.jar.Store(U("http://example.com/"), "a=2; Max-Age=0", {});
    Check("but only the cookie of that path", c.jar.size() == 1);
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; Max-Age=99999999999", {});
    c.now += 399 * 24h;
    Check("no cookie lives past 400 days", Header(c, "http://example.com/") == "a=1");
    c.now += 2 * 24h;
    Check("not even when it asks to", Header(c, "http://example.com/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; Max-Age=abc", {});
    c.jar.Store(U("http://example.com/"), "b=1; Max-Age=1.5", {});
    c.jar.Store(U("http://example.com/"), "c=1; Max-Age=", {});
    c.jar.Store(U("http://example.com/"), "d=1; Expires=not a date", {});
    c.now += 24h * 1000;
    Check("an invalid Max-Age or Expires is ignored, which leaves a session cookie", c.jar.size() == 4 && Header(c, "http://example.com/") == "a=1; b=1; c=1; d=1");
  }

  // ---- What a cookie says about itself ----
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=1; SECURE; httponly", {});
    Check("Secure over plain http is refused", c.jar.size() == 0);
    c.jar.Store(U("https://example.com/"), "a=1; SECURE; httponly", {});
    const auto both = c.jar.Matching(U("https://example.com/"), {});
    Check("over https it is kept, with its flags", both.size() == 1 && both[0].secure && both[0].httpOnly);
    Check("and goes only to https", Header(c, "https://example.com/") == "a=1" && Header(c, "http://example.com/").empty());
  }
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "a=secret; Secure", {});
    c.jar.Store(U("http://example.com/"), "a=planted", {});
    Check("plain http cannot overwrite a secure cookie", Header(c, "https://example.com/") == "a=secret" && c.jar.size() == 1);
    c.jar.Store(U("http://sub.example.com/"), "a=planted; Domain=example.com", {});
    Check("nor from a subdomain", Header(c, "https://example.com/") == "a=secret" && c.jar.size() == 1);
    c.jar.Store(U("https://example.com/"), "a=changed; Secure", {});
    Check("https can", Header(c, "https://example.com/") == "a=changed");
  }
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "__Secure-a=1", {});
    c.jar.Store(U("http://example.com/"), "__Secure-b=1; Secure", {});
    c.jar.Store(U("https://example.com/"), "__Secure-c=1; Secure", {});
    c.jar.Store(U("https://example.com/"), "__secure-d=1; Secure", {});
    c.jar.Store(U("https://example.com/"), "__secure-e=1", {});
    Check("__Secure- needs Secure, from a secure request", c.jar.size() == 2 && Header(c, "https://example.com/") == "__Secure-c=1; __secure-d=1");
  }
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "__Host-a=1; Secure; Path=/", {});
    Check("__Host- with everything it needs", Header(c, "https://example.com/") == "__Host-a=1");
    c.jar.Store(U("https://example.com/x/y"), "__Host-c=1; Secure", {});  // its default path is /x
    c.jar.Store(U("https://example.com/"), "__Host-d=1; Secure; Path=/; Domain=example.com", {});
    c.jar.Store(U("https://example.com/"), "__Host-e=1; Path=/", {});
    c.jar.Store(U("http://example.com/"), "__Host-f=1; Secure; Path=/", {});
    Check("__Host- without a root path, a Domain, Secure or https is refused", c.jar.size() == 1);
    c.jar.Store(U("https://example.com/"), "__Host-g", {});
    c.jar.Store(U("https://example.com/"), "__Secure-h; Secure", {});
    Check("a value that only looks like a prefix cannot do what the prefix guards", c.jar.size() == 1);
  }

  // ---- Same-site ----
  const CookieRequest crossSite{false, false};
  const CookieRequest crossSiteNavigation{false, true};
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "strict=1; SameSite=Strict", {});
    c.jar.Store(U("https://example.com/"), "lax=1; SameSite=Lax", {});
    c.jar.Store(U("https://example.com/"), "none=1; SameSite=None; Secure", {});
    c.jar.Store(U("https://example.com/"), "default=1", {});
    c.jar.Store(U("https://example.com/"), "weird=1; SameSite=Bogus", {});
    Check("same-site requests carry all of them", Header(c, "https://example.com/") == "strict=1; lax=1; none=1; default=1; weird=1");
    Check("a cross-site subrequest carries only None", Header(c, "https://example.com/", crossSite) == "none=1");
    Check("a cross-site navigation carries Lax and unspecified too", Header(c, "https://example.com/", crossSiteNavigation) == "lax=1; none=1; default=1; weird=1");
  }
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "a=1; SameSite=None", {});
    Check("SameSite=None needs Secure", c.jar.size() == 0);
  }
  {
    Clocked c;
    c.jar.Store(U("https://example.com/"), "strict=1; SameSite=Strict", crossSite);
    c.jar.Store(U("https://example.com/"), "lax=1; SameSite=Lax", crossSite);
    Check("a response to a cross-site request cannot set Strict or Lax cookies", c.jar.size() == 0);
    c.jar.Store(U("https://example.com/"), "none=1; SameSite=None; Secure", crossSite);
    c.jar.Store(U("https://example.com/"), "default=1", crossSite);
    Check("but can set the others", c.jar.size() == 2);
    Check("though an unspecified one is held to Lax", Header(c, "https://example.com/", crossSite) == "none=1");
  }

  // ---- Refused outright ----
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), std::string("a=b\x01"), {});
    c.jar.Store(U("http://example.com/"), std::string("a=b\x7f"), {});
    c.jar.Store(U("http://example.com/"), std::string("a=b; Path=/\x02"), {});
    c.jar.Store(U("http://example.com/"), "a=b\tc", {});
    Check("a control character spoils the whole header, except a tab", c.jar.size() == 1 && Header(c, "http://example.com/") == "a=b\tc");
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "a=" + std::string(4095, 'v'), {});  // 4096 with its name
    c.jar.Store(U("http://example.com/"), "b=" + std::string(4096, 'v'), {});
    Check("a name and value over 4096 bytes together are refused", c.jar.size() == 1 && Header(c, "http://example.com/").starts_with("a="));
    c.jar.Store(U("http://example.com/"), "", {});
    c.jar.Store(U("http://example.com/"), "=", {});
    c.jar.Store(U("http://example.com/"), " ; Path=/", {});
    Check("a header with nothing in it is no cookie", c.jar.size() == 1);
  }

  // ---- Limits ----
  {
    Clocked c;
    for (int i = 0; i < 200; ++i) {
      c.now += 1s;
      c.jar.Store(U("http://www.example.com/"), "k" + std::to_string(i) + "=v", {});
    }
    Check("a domain keeps 180 cookies", c.jar.size() == 180);
    const std::string header = Header(c, "http://www.example.com/");
    Check("and it is the oldest it drops", header.find("k0=") == std::string::npos && header.find("k19=") == std::string::npos && header.find("k20=") != std::string::npos &&
                                           header.find("k199=") != std::string::npos);
  }
  {
    Clocked c;
    for (int i = 0; i < 3200; ++i) {
      c.now += 1s;
      c.jar.Store(U("http://host" + std::to_string(i) + ".example/"), "k=v", {});  // a registrable domain each
    }
    Check("the jar keeps no more than 3000 in all", c.jar.size() == 3000);
  }
  {
    Clocked c;
    c.jar.Store(U("http://example.com/"), "keep=1", {});
    c.jar.Store(U("http://example.com/"), "old=1; Max-Age=5", {});
    for (int i = 0; i < 100; ++i) c.jar.Store(U("http://example.com/"), "fill" + std::to_string(i) + "=1", {});
    c.now += 10s;
    c.jar.Store(U("http://example.com/"), "new=1", {});
    Check("expired cookies are dropped when the jar is looked at", c.jar.size() == 102);
  }

  std::printf("cookies: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
