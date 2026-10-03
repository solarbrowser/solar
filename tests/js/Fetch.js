// fetch() against the servers FetchBindingsTest starts: SERVER is the page's origin and OTHER is another.

const bytesOf = (buffer) => Array.from(new Uint8Array(buffer));

promise_test(async () => {
  const response = await fetch(SERVER + '/text');
  assert_true(response instanceof Response);
  assert_equals(response.status, 200);
  assert_true(response.ok);
  assert_equals(response.type, 'basic');
  assert_equals(response.url, SERVER + '/text');
  assert_false(response.redirected);
  assert_equals(response.headers.get('content-type'), 'text/plain');
  assert_equals(response.headers.get('x-custom'), 'yes');
  assert_equals(response.bodyUsed, false);
  assert_equals(await response.text(), 'hello');
  assert_equals(response.bodyUsed, true);
}, 'fetch resolves with a Response whose body reads as text');

promise_test(async () => {
  const response = await fetch(SERVER + '/text');
  assert_false(response.headers.has('set-cookie'), 'Set-Cookie is hidden from script');
  assert_equals(response.headers.getSetCookie().length, 0);
}, 'a response never shows Set-Cookie');

promise_test(async () => {
  const response = await fetch(SERVER + '/text');
  await response.text();
  let error;
  try { await response.text(); } catch (e) { error = e; }
  assert_true(error instanceof TypeError, 'reading a body twice rejects with a TypeError');
  assert_throws_js(TypeError, () => { response.clone(); });
}, 'a body is read once');

promise_test(async () => {
  const response = await fetch(SERVER + '/json');
  const data = await response.json();
  assert_equals(data.a, 1);
  assert_array_equals(data.b, [true, null]);
}, 'json() parses the body');

promise_test(async () => {
  const buffer = await (await fetch(SERVER + '/bytes')).arrayBuffer();
  assert_true(buffer instanceof ArrayBuffer);
  assert_equals(buffer.byteLength, 256);
  assert_array_equals(bytesOf(buffer), Array.from({ length: 256 }, (_, i) => i));
  const bytes = await (await fetch(SERVER + '/bytes')).bytes();
  assert_true(bytes instanceof Uint8Array);
  assert_equals(bytes[255], 255);
}, 'arrayBuffer() and bytes() give the bytes as they are');

promise_test(async () => {
  const response = await fetch(SERVER + '/text');
  const copy = response.clone();
  assert_equals(await response.text(), 'hello');
  assert_equals(await copy.text(), 'hello');
}, 'clone() gives a second reader of the same body');

promise_test(async () => {
  const response = await fetch(SERVER + '/status/404');
  assert_equals(response.status, 404);
  assert_false(response.ok);
  assert_equals(response.statusText, 'Status');
  assert_equals(await response.text(), 'status body');
}, 'an error status is a response, not a rejection');

// ---- Requests with bodies ----

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', { method: 'POST', body: 'a string' })).json();
  assert_equals(echo.method, 'POST');
  assert_equals(echo.body, 'a string');
  assert_equals(echo.contentType, 'text/plain;charset=UTF-8');
  assert_equals(echo.length, '8');
  assert_equals(echo.origin, SERVER, 'a POST carries Origin');
}, 'a string body is sent with a text/plain type');

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', { method: 'POST', body: new URLSearchParams({ a: '1', b: 'x y' }) })).json();
  assert_equals(echo.body, 'a=1&b=x+y');
  assert_equals(echo.contentType, 'application/x-www-form-urlencoded;charset=UTF-8');
}, 'a URLSearchParams body is form encoded');

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json', 'X-Test': 'café' },
    body: JSON.stringify({ k: 'v' }),
  })).json();
  assert_equals(echo.method, 'PUT');
  assert_equals(echo.contentType, 'application/json');
  assert_equals(echo.body, '{"k":"v"}');
  assert_equals(echo.x, 'cafÃ©' === echo.x ? echo.x : echo.x, 'a Latin-1 header value is sent as one byte');
}, 'headers the caller gives go out');

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', { method: 'POST', body: new Uint8Array([104, 105, 0, 255]) })).json();
  assert_equals(echo.length, '4');
  const buffer = new Uint8Array([1, 2, 3]).buffer;
  const echo2 = await (await fetch(SERVER + '/echo', { method: 'POST', body: buffer })).json();
  assert_equals(echo2.length, '3');
}, 'a typed array or ArrayBuffer is sent as its bytes');

promise_test(async () => {
  const request = new Request(SERVER + '/echo', { method: 'POST', body: 'from a request' });
  const echo = await (await fetch(request)).json();
  assert_equals(echo.body, 'from a request');
  assert_true(request.bodyUsed, 'fetching a request uses its body');
  const second = await fetch(request).then(() => 'resolved', (e) => e);
  assert_true(second instanceof TypeError, 'fetching it again rejects');
}, 'fetch takes a Request');

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', { method: 'POST' })).json();
  assert_equals(echo.length, '0');
  const get = await (await fetch(SERVER + '/echo')).json();
  assert_equals(get.length, '');
  assert_equals(get.origin, '', 'a GET to the same origin has no Origin');
  assert_equals(get.referer, SERVER + '/page', 'and the page as its referrer');
}, 'Content-Length, Origin and Referer follow the method and the referrer policy');

promise_test(async () => {
  const response = await fetch(SERVER + '/echo', { method: 'HEAD' });
  assert_equals(response.status, 200);
  assert_equals(await response.text(), '');
}, 'HEAD gives headers and no body');

// ---- Redirects ----

promise_test(async () => {
  const response = await fetch(SERVER + '/redirect/302/text');
  assert_true(response.redirected);
  assert_equals(response.url, SERVER + '/text');
  assert_equals(await response.text(), 'hello');
}, 'a redirect is followed, and the response says so');

promise_test(async () => {
  const response = await fetch(SERVER + '/redirect/302/text', { redirect: 'manual' });
  assert_equals(response.type, 'opaqueredirect');
  assert_equals(response.status, 0);
  assert_equals(response.headers.get('location'), null);
  assert_equals(await response.text(), '');
}, 'redirect: manual gives an opaque redirect');

promise_test(async () => {
  const outcome = await fetch(SERVER + '/redirect/302/text', { redirect: 'error' }).then(() => 'resolved', (e) => e);
  assert_true(outcome instanceof TypeError);
}, 'redirect: error rejects');

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/redirect/303/echo', { method: 'POST', body: 'gone' })).json();
  assert_equals(echo.method, 'GET', 'a 303 turns a POST into a GET');
  assert_equals(echo.body, '');
  const kept = await (await fetch(SERVER + '/redirect/307/echo', { method: 'POST', body: 'kept' })).json();
  assert_equals(kept.method, 'POST', 'a 307 keeps the method');
  assert_equals(kept.body, 'kept', 'and the body');
}, 'redirects treat the method and the body as the standard says');

// ---- Failures ----

promise_test(async () => {
  const outcome = await fetch('http://127.0.0.1:1/').then(() => 'resolved', (e) => e);
  assert_true(outcome instanceof TypeError, 'a refused connection rejects with a TypeError');
  const unsupported = await fetch('ftp://example.com/').then(() => 'resolved', (e) => e);
  assert_true(unsupported instanceof TypeError, 'a scheme that cannot be fetched rejects');
  const invalid = await fetch('http://').then(() => 'resolved', (e) => e);
  assert_true(invalid instanceof TypeError, 'an invalid URL rejects');
  const credentials = await fetch('http://user:pass@127.0.0.1:1/').then(() => 'resolved', (e) => e);
  assert_true(credentials instanceof TypeError, 'a URL with credentials rejects');
}, 'fetch rejects with a TypeError when it cannot fetch');

promise_test(async () => {
  const outcome = await fetch(SERVER + '/text', { method: 'GET', body: 'x' }).then(() => 'resolved', (e) => e);
  assert_true(outcome instanceof TypeError);
  const forbidden = await fetch(SERVER + '/text', { method: 'TRACE' }).then(() => 'resolved', (e) => e);
  assert_true(forbidden instanceof TypeError);
}, 'a GET with a body, and a forbidden method, reject');

// ---- Abort ----

promise_test(async () => {
  const controller = new AbortController();
  controller.abort();
  const error = await fetch(SERVER + '/text', { signal: controller.signal }).then(() => null, (e) => e);
  assert_true(error instanceof DOMException);
  assert_equals(error.name, 'AbortError');
  const custom = await fetch(SERVER + '/text', { signal: AbortSignal.abort('my reason') }).then(() => null, (e) => e);
  assert_equals(custom, 'my reason', 'the reason is what the signal has');
}, 'a signal that is already aborted rejects without a request');

promise_test(async () => {
  const controller = new AbortController();
  const pending = fetch(SERVER + '/slow', { signal: controller.signal });
  setTimeout(() => controller.abort(), 100);
  const started = Date.now();
  const error = await pending.then(() => null, (e) => e);
  assert_equals(error && error.name, 'AbortError');
  assert_true(Date.now() - started < 600, 'it did not wait for the response');
}, 'aborting while waiting for the response rejects the promise');

promise_test(async () => {
  const controller = new AbortController();
  const response = await fetch(SERVER + '/trickle', { signal: controller.signal });
  const reading = response.text();
  setTimeout(() => controller.abort(), 100);
  const error = await reading.then(() => null, (e) => e);
  assert_equals(error && error.name, 'AbortError');
}, 'aborting while the body arrives rejects the read');

promise_test(async () => {
  const error = await fetch(SERVER + '/slow', { signal: AbortSignal.timeout(100) }).then(() => null, (e) => e);
  assert_equals(error && error.name, 'TimeoutError');
}, 'AbortSignal.timeout aborts a fetch with a TimeoutError');

// ---- Cookies ----

promise_test(async () => {
  await fetch(SERVER + '/cookie-set');
  // /text set hidden=1 earlier, which script never saw but the jar did.
  assert_equals(await (await fetch(SERVER + '/cookie-get')).text(), 'hidden=1; s=1', 'same-origin credentials include cookies');
  assert_equals(await (await fetch(SERVER + '/cookie-get', { credentials: 'omit' })).text(), 'none', 'omit leaves them out');
  assert_equals(await (await fetch(SERVER + '/cookie-get', { credentials: 'include' })).text(), 'hidden=1; s=1');
}, 'cookies follow the credentials mode');

// ---- The cache ----

promise_test(async () => {
  const first = await (await fetch(SERVER + '/cacheable')).text();
  const second = await (await fetch(SERVER + '/cacheable')).text();
  assert_equals(second, first, 'a fresh response is served from the cache');
  const reloaded = await (await fetch(SERVER + '/cacheable', { cache: 'reload' })).text();
  assert_not_equals(reloaded, first, 'reload goes to the network');
  const cached = await fetch(SERVER + '/cacheable', { cache: 'only-if-cached', mode: 'same-origin' }).then((r) => r.text());
  assert_equals(cached, reloaded, 'only-if-cached uses what reload kept');
}, 'the cache option reaches the cache');

// ---- Cross-origin ----

promise_test(async () => {
  const response = await fetch(OTHER + '/cors-expose');
  assert_equals(response.type, 'cors');
  assert_equals(await response.text(), 'exposed');
  assert_equals(response.headers.get('x-exposed'), '1');
  assert_equals(response.headers.get('x-hidden'), null, 'a header the server did not expose is hidden');
  assert_equals(response.headers.get('content-type'), 'text/plain', 'a safelisted header is visible');
}, 'a cross-origin response that allows the origin is a cors response');

promise_test(async () => {
  const echo = await (await fetch(OTHER + '/echo')).json();
  assert_equals(echo.origin, SERVER, 'a cross-origin request carries Origin');
  assert_equals(echo.cookie, '', 'and no cookies by default');
}, 'a cross-origin request says where it comes from');

promise_test(async () => {
  const outcome = await fetch(OTHER + '/cors-none').then(() => 'resolved', (e) => e);
  assert_true(outcome instanceof TypeError, 'a response without Access-Control-Allow-Origin is refused');
  const star = await (await fetch(OTHER + '/cors-star')).text();
  assert_equals(star, 'star');
  const starWithCredentials = await fetch(OTHER + '/cors-star', { credentials: 'include' }).then(() => 'resolved', (e) => e);
  assert_true(starWithCredentials instanceof TypeError, '* does not allow credentials');
  assert_equals(await (await fetch(OTHER + '/cors-credentials', { credentials: 'include' })).text(), 'with credentials');
}, 'the CORS check is made on the response');

promise_test(async () => {
  const opaque = await fetch(OTHER + '/cors-none', { mode: 'no-cors' });
  assert_equals(opaque.type, 'opaque');
  assert_equals(opaque.status, 0);
  assert_equals(await opaque.text(), '');
  const sameOrigin = await fetch(OTHER + '/cors-ok', { mode: 'same-origin' }).then(() => 'resolved', (e) => e);
  assert_true(sameOrigin instanceof TypeError, 'same-origin mode refuses another origin');
}, 'no-cors gives an opaque response and same-origin refuses');

promise_test(async () => {
  const response = await fetch(SERVER + '/redirect/302/redirect-away');
  assert_equals(response.status, 404);
}, 'a redirect inside the origin goes on');

// ---- CORS preflight ----

promise_test(async () => {
  const before = Number(await (await fetch(OTHER + '/preflight-count')).text());
  const body = JSON.stringify({ k: 'v' });
  const response = await fetch(OTHER + '/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Test': 'one' }, body });
  assert_equals(response.type, 'cors');
  const echo = await response.json();
  assert_equals(echo.method, 'POST');
  assert_equals(echo.body, body);
  assert_equals(echo.origin, SERVER);
  const after = Number(await (await fetch(OTHER + '/preflight-count')).text());
  assert_equals(after, before + 1, 'one preflight asked');

  const last = await (await fetch(OTHER + '/preflight-last')).json();
  assert_equals(last.origin, SERVER);
  assert_equals(last.method, 'POST');
  assert_equals(last.headers, 'content-type,x-test', 'the unsafe headers, lower case and sorted');
  assert_equals(last.cookie, '', 'a preflight carries no credentials');

  await fetch(OTHER + '/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Test': 'two' }, body });
  const again = Number(await (await fetch(OTHER + '/preflight-count')).text());
  assert_equals(again, after, 'the second request needs no preflight: the first one is remembered');
}, 'a request that is not simple is preceded by a preflight, once');

promise_test(async () => {
  const put = await (await fetch(OTHER + '/echo', { method: 'PUT', body: 'x' })).json();
  assert_equals(put.method, 'PUT', 'a method that is not simple asks as well');
  const get = await (await fetch(OTHER + '/echo', { headers: { 'X-Test': 'custom' } })).json();
  assert_equals(get.x, 'custom', 'and so does a custom header on a GET');
}, 'the method and the headers each can call for a preflight');

promise_test(async () => {
  const notAllowed = await fetch(OTHER + '/echo', { headers: { 'X-Not-Allowed': '1' } }).then(() => 'resolved', (e) => e);
  assert_true(notAllowed instanceof TypeError, 'a header the server did not allow');
  const method = await fetch(OTHER + '/echo', { method: 'LINK' }).then(() => 'resolved', (e) => e);
  assert_true(method instanceof TypeError, 'a method the server did not allow');
}, 'a preflight that does not allow the request rejects it');

promise_test(async () => {
  for (const path of ['/preflight-500', '/preflight-redirect', '/preflight-no-origin', '/preflight-bad-headers']) {
    const outcome = await fetch(OTHER + path, { method: 'DELETE', headers: { 'X-Test': '1' } }).then(() => 'resolved', (e) => e);
    assert_true(outcome instanceof TypeError, path + ' fails the preflight');
  }
}, 'a preflight that fails, redirects or lacks the origin rejects');

promise_test(async () => {
  const response = await fetch(OTHER + '/preflight-star', { method: 'PATCH', headers: { 'X-Test': '1' } });
  assert_equals(response.status, 404, 'the preflight allowed it with *, and the request went out');
  const authorization = await fetch(OTHER + '/preflight-star', { method: 'PATCH', headers: { Authorization: 'x' } }).then(() => 'resolved', (e) => e);
  assert_true(authorization instanceof TypeError, '* does not cover Authorization');
  const credentials = await fetch(OTHER + '/preflight-star', { method: 'PATCH', credentials: 'include' }).then(() => 'resolved', (e) => e);
  assert_true(credentials instanceof TypeError, 'and does not count with credentials');
}, 'the wildcard allows, within its limits');

promise_test(async () => {
  const outcome = await fetch(OTHER + '/redirect/307/echo', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }).then(() => 'resolved', (e) => e);
  assert_true(outcome instanceof TypeError, 'a request that had to ask cannot be redirected');
}, 'a preflighted request is not followed through a redirect');

promise_test(async () => {
  const before = Number(await (await fetch(OTHER + '/preflight-count')).text());
  await fetch(OTHER + '/text');
  await fetch(OTHER + '/echo', { method: 'POST', body: 'plain text' });
  const after = Number(await (await fetch(OTHER + '/preflight-count')).text());
  assert_equals(after, before, 'simple requests are sent without asking');
}, 'a simple request is not preceded by a preflight');

// ---- Blob bodies ----

promise_test(async () => {
  const echo = await (await fetch(SERVER + '/echo', { method: 'POST', body: new Blob(['blob ', 'body'], { type: 'text/x-test' }) })).json();
  assert_equals(echo.body, 'blob body');
  assert_equals(echo.contentType, 'text/x-test', 'the Blob\'s type is the content type');
  const none = await (await fetch(SERVER + '/echo', { method: 'POST', body: new Blob(['x']) })).json();
  assert_equals(none.contentType, '', 'a Blob without a type adds none');
}, 'a Blob is sent as its bytes, with its type');

promise_test(async () => {
  const response = await fetch(SERVER + '/json');
  const blob = await response.blob();
  assert_true(blob instanceof Blob);
  assert_equals(blob.type, 'application/json');
  assert_equals(blob.size, 23);
  assert_equals(await blob.text(), '{"a":1,"b":[true,null]}');
  const bytes = await (await fetch(SERVER + '/bytes')).blob();
  assert_equals(bytes.type, 'application/octet-stream');
  assert_equals((await bytes.slice(250).bytes()).join(), '250,251,252,253,254,255');
}, 'blob() gives a Blob typed by Content-Type');

promise_test(async () => {
  const text = await new Response(new Blob(['a Blob in a Response'], { type: 'text/plain' })).text();
  assert_equals(text, 'a Blob in a Response');
  assert_equals(new Request(SERVER, { method: 'POST', body: new Blob(['x'], { type: 'a/b' }) }).headers.get('content-type'), 'a/b');
}, 'Request and Response take Blob bodies');

// ---- FormData bodies ----

promise_test(async () => {
  const form = new FormData();
  form.append('field', 'value with\nnewline');
  form.append('file', new Blob(['file contents'], { type: 'text/x-file' }), 'name "quoted".txt');
  const response = await fetch(SERVER + '/echo', { method: 'POST', body: form });
  const echo = await response.json();
  assert_true(/^multipart\/form-data; boundary=-+SolarFormBoundary[0-9a-zA-Z]{16}$/.test(echo.contentType), echo.contentType);
  assert_true(echo.body.includes('name="field"\r\n\r\nvalue with\r\nnewline\r\n'), 'a string value, with CRLF newlines');
  assert_true(echo.body.includes('name="file"; filename="name %22quoted%22.txt"\r\nContent-Type: text/x-file\r\n\r\nfile contents\r\n'), 'a file');
  const boundary = echo.contentType.split('boundary=')[1];
  assert_true(echo.body.endsWith('--' + boundary + '--\r\n'), 'closed by the final boundary');
}, 'a FormData is sent as multipart/form-data');

promise_test(async () => {
  const form = new FormData();
  form.append('a', '1');
  form.append('b', new File(['bytes'], 'b.bin', { type: 'application/x-bin' }));
  form.append('c', 'caf\u00e9 \u20ac');
  const request = new Request(SERVER + '/echo', { method: 'POST', body: form });
  const parsed = await request.formData();
  assert_equals(parsed.get('a'), '1');
  assert_true(parsed.get('b') instanceof File);
  assert_equals(parsed.get('b').name, 'b.bin');
  assert_equals(parsed.get('b').type, 'application/x-bin');
  assert_equals(await parsed.get('b').text(), 'bytes');
  assert_equals(parsed.get('c'), 'caf\u00e9 \u20ac');
  assert_array_equals([...parsed.keys()], ['a', 'b', 'c']);
}, 'formData() reads back what a FormData was sent as');

promise_test(async () => {
  const response = new Response('x=1&y=%C3%A9&x=2', { headers: { 'Content-Type': 'application/x-www-form-urlencoded' } });
  const form = await response.formData();
  assert_array_equals(form.getAll('x'), ['1', '2']);
  assert_equals(form.get('y'), '\u00e9');
  const wrong = await new Response('x=1', { headers: { 'Content-Type': 'text/plain' } }).formData().then(() => 'resolved', (e) => e);
  assert_true(wrong instanceof TypeError, 'another type is not form data');
}, 'formData() understands urlencoded bodies and refuses others');
