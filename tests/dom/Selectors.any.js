const document = new Text().ownerDocument;
const root = new DOMParser().parseFromString(`<!DOCTYPE html>
<div id="a" class="one two" data-x="Hello World" lang="en-US">
  <p class="first">one</p>
  <p>two <em class="e">em</em></p>
  <!-- c --><span></span>
  <ul><li>1</li><li class="sel">2</li><li>3</li><li>4</li></ul>
  <input type="CHECKBOX" checked disabled>
  <a href="#x">link</a>
</div>`, 'text/html');
const q = (s) => root.querySelector(s);
const all = (s) => Array.from(root.querySelectorAll(s));

test(() => {
  assert_equals(q('#a').id, 'a');
  assert_equals(all('p').length, 2);
  assert_equals(all('P').length, 2, 'type selectors do not mind the case of HTML');
  assert_equals(q('.first').textContent, 'one');
  assert_equals(all('.one.two').length, 1);
  assert_equals(all('*').length, root.getElementsByTagName('*').length);
  assert_equals(q('div#a.one'), q('#a'));
  assert_equals(q('.nothing'), null);
}, 'simple selectors');

test(() => {
  assert_equals(all('[data-x]').length, 1);
  assert_equals(all('[data-x="Hello World"]').length, 1);
  assert_equals(all('[data-x=hello]').length, 0);
  assert_equals(all('[data-x="hello world" i]').length, 1);
  assert_equals(all('[data-x^="Hello"]').length, 1);
  assert_equals(all('[data-x$="World"]').length, 1);
  assert_equals(all('[data-x*="o W"]').length, 1);
  assert_equals(all('[class~="two"]').length, 1);
  assert_equals(all('[lang|="en"]').length, 1);
  assert_equals(all('[type=checkbox]').length, 1, 'type is a case-insensitive attribute in HTML');
  assert_equals(all('[DATA-X]').length, 1);
  assert_equals(all('[data-x=""]').length, 0);
  assert_equals(all('[data-x^=""]').length, 0);
}, 'attribute selectors');

test(() => {
  assert_equals(all('div p').length, 2);
  assert_equals(all('div > p').length, 2);
  assert_equals(all('div > em').length, 0);
  assert_equals(all('p + p').length, 1);
  assert_equals(all('p ~ span').length, 1);
  assert_equals(all('p.first + p > em').length, 1);
  assert_equals(all('ul li:first-child').length, 1);
  assert_equals(all('div>p').length, 2, 'combinators without space');
}, 'combinators');

test(() => {
  assert_equals(q('li:nth-child(2)').textContent, '2');
  assert_equals(all('li:nth-child(odd)').length, 2);
  assert_equals(all('li:nth-child(even)').length, 2);
  assert_equals(all('li:nth-child(n+3)').length, 2);
  assert_equals(all('li:nth-child(-n+2)').length, 2);
  assert_equals(all('li:nth-child(2n+1)').length, 2);
  assert_equals(all('li:nth-child( 2n + 1 )').length, 2);
  assert_equals(q('li:nth-last-child(1)').textContent, '4');
  assert_equals(all('li:nth-of-type(3)').length, 1);
  assert_equals(all('li:nth-child(1 of .sel)').length, 1, 'counted among those that match the selector');
  assert_equals(all('li:nth-child(2 of .sel)').length, 0);
  assert_equals(all('li:last-child').length, 1);
  assert_equals(all('p:only-of-type').length, 0);
  assert_equals(all('span:only-of-type').length, 1);
  assert_equals(all('span:empty').length, 1);
  assert_equals(all(':root').length, 1);
  assert_equals(q(':root').localName, 'html');
}, 'structural pseudo-classes');

test(() => {
  assert_equals(all('p:not(.first)').length, 1);
  assert_equals(all(':is(p, span)').length, 3);
  assert_equals(all(':where(p, span)').length, 3);
  assert_equals(all('div:has(> p.first)').length, 1);
  assert_equals(all('div:has(em)').length, 1);
  assert_equals(all('p:has(+ p)').length, 1);
  assert_equals(all('p:has(~ span)').length, 2);
  assert_equals(all(':is(:invalid-name, p)').length, 2, ':is() drops what it cannot parse');
  assert_equals(all('li:not(:first-child, :last-child)').length, 2);
}, 'functional pseudo-classes');

test(() => {
  assert_equals(all('input:checked').length, 1);
  assert_equals(all('input:disabled').length, 1);
  assert_equals(all('input:enabled').length, 0);
  assert_equals(all('a:link').length, 1);
  assert_equals(all('a:any-link').length, 1);
  assert_equals(all('a:hover').length, 0);
  assert_equals(all(':lang(en)').length > 0, true);
  assert_equals(all('p:lang(en)').length, 2);
  assert_equals(all(':lang(fr)').length, 0);
  assert_equals(all('p::before').length, 0, 'a pseudo-element is no element');
}, 'state pseudo-classes');

test(() => {
  const div = q('#a');
  assert_equals(div.querySelectorAll(':scope > p').length, 2);
  assert_equals(div.querySelector('div'), null, 'the element itself is not among its descendants');
  assert_true(q('.first').matches('div > p.first'));
  assert_false(q('.first').matches('.second'));
  assert_equals(q('.e').closest('p'), q('.e').parentNode);
  assert_equals(q('.e').closest('div'), div);
  assert_equals(q('.e').closest('.none'), null);
  assert_equals(q('.e').closest('em'), q('.e'), 'closest includes the element');
  assert_true(q('.sel').webkitMatchesSelector('li'));
  const fragment = document.createDocumentFragment();
  fragment.append(document.createElement('b'));
  assert_equals(fragment.querySelector('b'), fragment.firstChild);
  assert_equals(fragment.querySelectorAll('b').length, 1);
}, 'querySelector, matches and closest');

test(() => {
  for (const bad of ['', ' ', 'p,', ',p', 'p >', '> p', 'p..a', '#', '.', '[', '[a=]', '[a=1]', ':nth-child()', ':nth-child(2n+)', 'a:unknown', '::unknown',
                     'ns|p', ':not()x', 'p q:', '1a', '#1a', ':has()', '[a|]', 'p ||']) {
    assert_throws_dom('SyntaxError', () => root.querySelector(bad), 'selector ' + JSON.stringify(bad));
  }
  assert_throws_js(TypeError, () => root.querySelector());
  assert_equals(all('*|p').length, 2);
  assert_equals(all('p\\:x').length, 0);
  assert_equals(all('#\\61').length, 1, '\\61 is a');
  assert_equals(all('\\70').length, 2, 'an escape in a name');
}, 'invalid selectors and escapes');

test(() => {
  assert_equals(CSS.escape('a b'), 'a\\ b');
  assert_equals(CSS.escape('1a'), '\\31 a');
  assert_equals(CSS.escape('-1'), '-\\31 ');
  assert_equals(CSS.escape('-'), '\\-');
  assert_equals(CSS.escape('\0'), '�');
  assert_equals(CSS.escape('é_-9'), 'é_-9');
  assert_equals(CSS.escape('a\x01b'), 'a\\1 b');
}, 'CSS.escape');
