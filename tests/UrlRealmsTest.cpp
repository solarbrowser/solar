#include <cstdio>
#include <string>

#include "solar/web/UrlBindings.h"

namespace {

namespace qe = Quanta::Embed;
using Quanta::Value;

bool Run(qe::Realm& realm, const std::string& source, const char* name) {
  qe::EvaluateResult result = realm.Evaluate(source, name);
  if (!result.ok) std::printf("FAIL %s: %s\n", name, result.error.c_str());
  return result.ok;
}

// Hands the global `name` of one realm to another, as a host does between documents.
void Pass(qe::Realm& from, qe::Realm& to, const char* name) {
  Quanta::Context& source = from.GetContext();
  Quanta::Context& target = to.GetContext();
  qe::Set(target, Value(target.get_global_object()), name,
          qe::Get(source, Value(source.get_global_object()), name));
}

const char* kChecks = R"js(
let failures = 0;
function check(name, condition) {
  if (condition) return;
  failures++;
  console.log('FAIL ' + name);
}
function threw(fn) { try { fn(); } catch (e) { return e; } return null; }
function done() { if (failures) throw new Error(failures + ' checks failed'); }
)js";

}  // namespace

int main() {
  auto isolate = qe::Isolate::Create();
  auto a = isolate->CreateRealm();
  auto b = isolate->CreateRealm();
  solar::web::InstallUrlApis(*a);
  solar::web::InstallUrlApis(*b);

  bool ok = Run(*a, "globalThis.A = { URL, URLSearchParams, TypeError, url: new URL('https://a.example/?k=v') };", "setup a");
  Pass(*a, *b, "A");
  ok = Run(*b, std::string(kChecks) + R"js(
    check('each realm has its own URL', URL !== A.URL && URLSearchParams !== A.URLSearchParams);
    check('a URL belongs to its own realm', A.url instanceof A.URL && !(A.url instanceof URL));
    check('prototype is its realm\'s', Object.getPrototypeOf(A.url) === A.URL.prototype);
    check('a foreign getter works', A.url.href === 'https://a.example/?k=v');

    check('query object has the defining realm\'s prototype',
          A.url.searchParams instanceof A.URLSearchParams && !(A.url.searchParams instanceof URLSearchParams));
    check('foreign iteration works', JSON.stringify([...A.url.searchParams]) === '[["k","v"]]');

    const own = new URL('https://b.example/?x=1');
    check('constructs in its own realm', own instanceof URL && !(own instanceof A.URL));
    check('static methods use their realm', URL.parse('https://b.example/') instanceof URL);
    check('foreign static methods use theirs', A.URL.parse('https://a.example/') instanceof A.URL);
    check('foreign constructor makes foreign objects', new A.URL('https://a.example/') instanceof A.URL);
    check('foreign URLSearchParams', new A.URLSearchParams('q=1') instanceof A.URLSearchParams);
    check('own query object', own.searchParams instanceof URLSearchParams && !(own.searchParams instanceof A.URLSearchParams));

    class Sub extends A.URL {}
    check('subclassing across realms', new Sub('https://a.example/') instanceof Sub);

    const mine = threw(() => new URL('nope'));
    check('own native throws its own error', mine instanceof TypeError && !(mine instanceof A.TypeError));
    const theirs = threw(() => new A.URL('nope'));
    check('foreign native throws its realm\'s error', theirs instanceof A.TypeError && !(theirs instanceof TypeError));
    done();
  )js", "two realms") && ok;

  // What a destroyed realm made stays usable while something still holds it. Only the URL is
  // kept; the rest of what b held from the realm is let go.
  ok = Run(*b, "const kept = A.url; A = { url: kept };", "release") && ok;
  a.reset();
  isolate->CollectGarbage();
  ok = Run(*b, R"js(
    failures = 0;
    check('a URL outlives its realm', A.url.href === 'https://a.example/?k=v');
    check('its query object is made after the realm is gone', A.url.searchParams.get('k') === 'v');
    check('and iterates', JSON.stringify([...A.url.searchParams.entries()]) === '[["k","v"]]');
    A.url.searchParams.append('n', '1');
    check('and still writes through', A.url.search === '?k=v&n=1');
    done();
  )js", "after destroying a") && ok;

  std::printf("realms: %s\n", ok ? "passed" : "FAILED");
  return ok ? 0 : 1;
}
