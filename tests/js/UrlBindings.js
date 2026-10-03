let total = 0;
let failed = 0;

function check(name, got, want) {
  total++;
  if (Object.is(got, want)) return;
  failed++;
  console.log('FAIL ' + name + ': want ' + String(want) + ', got ' + String(got));
}

function throwsTypeError(name, fn) {
  total++;
  try {
    fn();
  } catch (e) {
    if (e instanceof TypeError) return;
    failed++;
    console.log('FAIL ' + name + ': threw ' + e);
    return;
  }
  failed++;
  console.log('FAIL ' + name + ': did not throw');
}

const u = new URL('https://user:pw@Example.com:8080/a/b/../c?x=1&y=2#frag');
check('href', u.href, 'https://user:pw@example.com:8080/a/c?x=1&y=2#frag');
check('origin', u.origin, 'https://example.com:8080');
check('protocol', u.protocol, 'https:');
check('username', u.username, 'user');
check('password', u.password, 'pw');
check('host', u.host, 'example.com:8080');
check('hostname', u.hostname, 'example.com');
check('port', u.port, '8080');
check('pathname', u.pathname, '/a/c');
check('search', u.search, '?x=1&y=2');
check('hash', u.hash, '#frag');
check('toString', u.toString(), u.href);
check('toJSON', JSON.stringify({ u }), '{"u":"' + u.href + '"}');
check('class string', Object.prototype.toString.call(u), '[object URL]');

check('relative with base', new URL('../x?q#h', 'https://a.example/b/c/d').href, 'https://a.example/b/x?q#h');
check('base undefined', new URL('http://a.example/', undefined).href, 'http://a.example/');
check('idna', new URL('https://faß.example/').hostname, 'xn--fa-hia.example');
check('ipv4', new URL('http://0x7f.1/').hostname, '127.0.0.1');

throwsTypeError('invalid url', () => new URL('not a url'));
throwsTypeError('invalid base', () => new URL('/x', 'not a url'));
throwsTypeError('missing argument', () => new URL());
throwsTypeError('called without new', () => URL('https://a.example/'));
throwsTypeError('illegal invocation', () => Object.getOwnPropertyDescriptor(URL.prototype, 'href').get.call({}));

check('parse ok', URL.parse('https://a.example/').href, 'https://a.example/');
check('parse null', URL.parse('nope'), null);
check('parse with base', URL.parse('/p', 'https://a.example/').href, 'https://a.example/p');
check('canParse true', URL.canParse('https://a.example/'), true);
check('canParse false', URL.canParse('nope'), false);

const s = new URL('https://example.com/p?a=1');
s.protocol = 'http'; check('set protocol', s.href, 'http://example.com/p?a=1');
s.username = 'u'; s.password = 'p'; check('set credentials', s.href, 'http://u:p@example.com/p?a=1');
s.host = 'other.example:81'; check('set host', s.host, 'other.example:81');
s.hostname = 'third.example'; check('set hostname', s.host, 'third.example:81');
s.port = ''; check('set port', s.host, 'third.example');
s.pathname = '/x y'; check('set pathname', s.pathname, '/x%20y');
s.search = '?k=v w'; check('set search', s.search, '?k=v%20w');
s.hash = 'top'; check('set hash', s.hash, '#top');
s.href = 'https://reset.example/'; check('set href', s.href, 'https://reset.example/');
throwsTypeError('set bad href', () => { s.href = 'nope'; });
check('href kept after bad set', s.href, 'https://reset.example/');

class SubUrl extends URL { extra() { return 'sub:' + this.pathname; } }
const sub = new SubUrl('https://a.example/q');
check('subclass instance', sub instanceof SubUrl && sub instanceof URL, true);
check('subclass method', sub.extra(), 'sub:/q');

// searchParams is the same object each time and follows the URL both ways.
const link = new URL('https://example.com/?a=1&b=2');
const sp = link.searchParams;
check('same object', link.searchParams === sp, true);
check('sp get', sp.get('b'), '2');
sp.append('c', '3 4');
check('append updates url', link.search, '?a=1&b=2&c=3+4');
sp.delete('a');
check('delete updates url', link.href, 'https://example.com/?b=2&c=3+4');
link.search = '?z=9';
check('search setter updates list', sp.get('z'), '9');
check('search setter drops old', sp.has('b'), false);
link.href = 'https://example.com/?q=r';
check('href setter updates list', sp.get('q'), 'r');
sp.delete('q');
check('emptied list drops the ?', link.href, 'https://example.com/');

const params = new URLSearchParams('?a=1&b=2&a=3');
check('size', params.size, 3);
check('get first', params.get('a'), '1');
check('get missing', params.get('zz'), null);
check('getAll', params.getAll('a').join(','), '1,3');
check('has', params.has('b'), true);
check('has with value', params.has('a', '3'), true);
check('has with wrong value', params.has('a', '9'), false);
params.set('a', 'X');
check('set', params.toString(), 'a=X&b=2');
params.append('c', '&');
check('serialize escapes', String(params), 'a=X&b=2&c=%26');
params.delete('a', 'nope');
check('delete with value keeps others', params.has('a'), true);
check('empty', new URLSearchParams().toString(), '');
check('undefined init', new URLSearchParams(undefined).size, 0);
check('string init', new URLSearchParams('x=1&x=2').size, 2);

check('record init', new URLSearchParams({ a: '1', b: '2' }).toString(), 'a=1&b=2');
throwsTypeError('record with an enumerable symbol key', () => new URLSearchParams({ a: '1', [Symbol('s')]: '2' }));
check('record skips non-enumerable symbols', new URLSearchParams(Object.defineProperty({ a: '1' }, Symbol('s'), { value: '2', enumerable: false })).toString(), 'a=1');
check('record converts values', new URLSearchParams({ n: 5, t: true, z: null }).toString(), 'n=5&t=true&z=null');
check('sequence init', new URLSearchParams([['a', '1'], ['b', '2']]).toString(), 'a=1&b=2');
check('sequence keeps duplicates', new URLSearchParams([['a', '1'], ['a', '2']]).toString(), 'a=1&a=2');
check('map init', new URLSearchParams(new Map([['m', 'n']])).toString(), 'm=n');
check('params init', new URLSearchParams(new URLSearchParams('p=q')).toString(), 'p=q');
check('generator init', new URLSearchParams((function* () { yield ['g', 'h']; })()).toString(), 'g=h');
check('null init is a string', new URLSearchParams(null).toString(), 'null=');
check('number init is a string', new URLSearchParams(5).toString(), '5=');
check('init object is not linked', (() => { const o = { a: '1' }; const p = new URLSearchParams(o); o.a = '2'; return p.get('a'); })(), '1');
throwsTypeError('pair of three', () => new URLSearchParams([['a', 'b', 'c']]));
throwsTypeError('pair of one', () => new URLSearchParams([['a']]));
throwsTypeError('pair is not an object', () => new URLSearchParams(['ab']));
throwsTypeError('pair is not iterable', () => new URLSearchParams([{}]));
throwsTypeError('iterator method not callable', () => new URLSearchParams({ [Symbol.iterator]: 1 }));
throwsTypeError('iterator throws', () => new URLSearchParams({ *[Symbol.iterator]() { throw new TypeError('x'); } }));

const sorted = new URLSearchParams('z=1&a=2&\u{1F308}=3&ﬃ=4');
sorted.sort();
check('sort by code unit', [...sorted.keys()].join('|'), ['a', 'z', '\u{1F308}', '\uFB03'].join('|'));

const iter = new URLSearchParams('a=1&b=2');
check('iterator is entries', iter[Symbol.iterator] === iter.entries, true);
check('spread', JSON.stringify([...iter]), '[["a","1"],["b","2"]]');
check('keys', [...iter.keys()].join(), 'a,b');
check('values', [...iter.values()].join(), '1,2');
let seen = '';
for (const [k, v] of iter) seen += k + v;
check('for-of', seen, 'a1b2');
const each = [];
const thisArg = {};
iter.forEach(function (v, k, p) { each.push(k + v + (p === iter) + (this === thisArg)); }, thisArg);
check('forEach', each.join(), 'a1truetrue,b2truetrue');
check('iterator tag', Object.prototype.toString.call(iter.entries()), '[object URLSearchParams Iterator]');
const live = new URLSearchParams('a=1');
const liveIter = live.keys();
live.append('b', '2');
check('iterator is live', [...liveIter].join(), 'a,b');
throwsTypeError('forEach needs a function', () => iter.forEach(1));
throwsTypeError('append needs two arguments', () => iter.append('x'));
throwsTypeError('params illegal invocation', () => URLSearchParams.prototype.get.call({}, 'a'));

console.log('bindings: ' + (total - failed) + '/' + total + ' passed');
if (failed) throw new Error(failed + ' checks failed');
