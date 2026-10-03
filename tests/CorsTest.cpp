#include <cstdio>
#include <string>

#include "solar/net/Cors.h"

namespace {

using namespace solar::net;
using namespace std::chrono_literals;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

HttpResponseHead Response(int status, std::vector<std::pair<std::string, std::string>> headers) {
  HttpResponseHead head;
  head.status = status;
  head.headers = std::move(headers);
  return head;
}

FetchHeaders Headers(std::initializer_list<std::pair<const char*, const char*>> list) {
  FetchHeaders headers;
  for (const auto& [name, value] : list) headers.AppendUnchecked(name, value);
  return headers;
}

}  // namespace

int main() {
  const std::string origin = "http://page.test:8000";

  // The check on a response.
  Check("the origin named", CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin}}), origin, false));
  Check("any origin, without credentials", CorsAllowsResponse(Response(200, {{"access-control-allow-origin", "*"}}), origin, false));
  Check("but not with them", !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", "*"}}), origin, true));
  Check("another origin is refused", !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", "http://other.test"}}), origin, false));
  Check("no header is refused", !CorsAllowsResponse(Response(200, {}), origin, false));
  Check("so are two", !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin}, {"Access-Control-Allow-Origin", origin}}), origin, false));
  Check("credentials need Allow-Credentials: true",
        CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin}, {"Access-Control-Allow-Credentials", "true"}}), origin, true) &&
            !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin}}), origin, true) &&
            !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin}, {"Access-Control-Allow-Credentials", "True"}}), origin, true));
  Check("a null origin is matched as the string", CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", "null"}}), "null", false));
  Check("the origin is compared whole", !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", origin + "/"}}), origin, false) &&
                                        !CorsAllowsResponse(Response(200, {{"Access-Control-Allow-Origin", "HTTP://PAGE.TEST:8000"}}), origin, false));

  // What a script may see.
  {
    const auto head = Response(200, {{"Access-Control-Expose-Headers", "X-One, x-two"}});
    Check("safelisted headers are exposed", IsCorsExposedHeader("Content-Type", head, false) && IsCorsExposedHeader("expires", head, true));
    Check("named ones too, in any case", IsCorsExposedHeader("x-one", head, false) && IsCorsExposedHeader("X-TWO", head, false));
    Check("others are not", !IsCorsExposedHeader("x-three", head, false) && !IsCorsExposedHeader("set-cookie", head, false));
    const auto star = Response(200, {{"Access-Control-Expose-Headers", "*"}});
    Check("* exposes all without credentials only", IsCorsExposedHeader("x-any", star, false) && !IsCorsExposedHeader("x-any", star, true));
  }

  // Whether to ask first.
  Check("the simple methods and headers need no preflight", !NeedsPreflight("GET", Headers({{"Accept", "*/*"}, {"Content-Type", "text/plain"}})) &&
                                                             !NeedsPreflight("POST", Headers({{"Content-Type", "application/x-www-form-urlencoded"}})));
  Check("another method does", NeedsPreflight("PUT", Headers({})) && NeedsPreflight("DELETE", Headers({})) && NeedsPreflight("PATCH", Headers({})));
  Check("a JSON body does", NeedsPreflight("POST", Headers({{"Content-Type", "application/json"}})));
  Check("a custom header does", NeedsPreflight("GET", Headers({{"X-Custom", "1"}})) && NeedsPreflight("GET", Headers({{"Authorization", "x"}})));
  Check("unsafe names are lower case, sorted and unique",
        CorsUnsafeRequestHeaderNames(Headers({{"X-B", "1"}, {"x-a", "2"}, {"X-B", "3"}, {"Accept", "*/*"}, {"Content-Type", "application/json"}})) ==
            std::vector<std::string>({"content-type", "x-a", "x-b"}));

  // The verdict on a preflight response.
  const auto ok = [&](std::vector<std::pair<std::string, std::string>> headers, int status = 204) {
    headers.insert(headers.begin(), {"Access-Control-Allow-Origin", origin});
    return Response(status, headers);
  };
  {
    const auto verdict = CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "PUT, delete"}, {"Access-Control-Allow-Headers", "X-A, Content-Type"}}), origin, false, "PUT", {"content-type", "x-a"});
    Check("methods and headers named are allowed", verdict.ok, verdict.reason);
    Check("the permission is kept", verdict.permission.AllowsMethod("PUT") && verdict.permission.AllowsMethod("GET") && verdict.permission.AllowsHeader("x-a") &&
                                    !verdict.permission.AllowsHeader("x-b") && !verdict.permission.AllowsMethod("PATCH"));
  }
  Check("a method not named is refused", !CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "PUT"}}), origin, false, "DELETE", {}).ok);
  Check("methods are case sensitive", !CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "delete"}}), origin, false, "DELETE", {}).ok);
  Check("a header not named is refused", !CheckPreflightResponse(ok({{"Access-Control-Allow-Headers", "x-a"}}), origin, false, "GET", {"x-a", "x-b"}).ok);
  Check("a simple method needs no naming", CheckPreflightResponse(ok({{"Access-Control-Allow-Headers", "x-a"}}), origin, false, "POST", {"x-a"}).ok);
  Check("a failing status is refused", !CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "PUT"}}, 500), origin, false, "PUT", {}).ok &&
                                        !CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "PUT"}}, 301), origin, false, "PUT", {}).ok);
  Check("a response without the origin is refused", !CheckPreflightResponse(Response(204, {{"Access-Control-Allow-Methods", "PUT"}}), origin, false, "PUT", {}).ok);
  Check("* covers methods and headers without credentials",
        CheckPreflightResponse(ok({{"Access-Control-Allow-Methods", "*"}, {"Access-Control-Allow-Headers", "*"}}), origin, false, "PATCH", {"x-a"}).ok);
  Check("not with them", !CheckPreflightResponse(Response(204, {{"Access-Control-Allow-Origin", origin}, {"Access-Control-Allow-Credentials", "true"}, {"Access-Control-Allow-Methods", "*"}}),
                                                 origin, true, "PATCH", {}).ok);
  Check("* never covers Authorization", !CheckPreflightResponse(ok({{"Access-Control-Allow-Headers", "*"}}), origin, false, "GET", {"authorization"}).ok &&
                                         CheckPreflightResponse(ok({{"Access-Control-Allow-Headers", "*, Authorization"}}), origin, false, "GET", {"authorization"}).ok);
  {
    const auto five = CheckPreflightResponse(ok({}), origin, false, "GET", {});
    const auto sixty = CheckPreflightResponse(ok({{"Access-Control-Max-Age", "60"}}), origin, false, "GET", {});
    const auto huge = CheckPreflightResponse(ok({{"Access-Control-Max-Age", "999999"}}), origin, false, "GET", {});
    const auto bad = CheckPreflightResponse(ok({{"Access-Control-Max-Age", "soon"}}), origin, false, "GET", {});
    Check("Max-Age: 5 seconds by default, as given, at most two hours",
          five.permission.maxAge == 5s && sixty.permission.maxAge == 60s && huge.permission.maxAge == 7200s && bad.permission.maxAge == 5s);
  }

  // The cache.
  {
    auto now = std::chrono::steady_clock::time_point{} + 1h;
    PreflightCache cache([&] { return now; });
    PreflightPermission permission;
    permission.methods = {"PUT"};
    permission.headers = {"x-a"};
    permission.maxAge = 60s;
    cache.Store(origin, "http://api.test/x", false, permission);
    Check("a request it covers is allowed", cache.Allows(origin, "http://api.test/x", false, "PUT", {"x-a"}));
    Check("one it does not is not", !cache.Allows(origin, "http://api.test/x", false, "DELETE", {}) && !cache.Allows(origin, "http://api.test/x", false, "PUT", {"x-b"}));
    Check("another URL, origin or credentials mode is not covered", !cache.Allows(origin, "http://api.test/y", false, "PUT", {}) && !cache.Allows("http://o.test", "http://api.test/x", false, "PUT", {}) &&
                                                                    !cache.Allows(origin, "http://api.test/x", true, "PUT", {}));
    now += 59s;
    Check("until it expires", cache.Allows(origin, "http://api.test/x", false, "PUT", {}));
    now += 2s;
    Check("and no longer", !cache.Allows(origin, "http://api.test/x", false, "PUT", {}));
    permission.maxAge = 0s;
    cache.Store(origin, "http://api.test/z", false, permission);
    Check("Max-Age: 0 keeps nothing", !cache.Allows(origin, "http://api.test/z", false, "PUT", {}));
  }

  std::printf("cors: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
