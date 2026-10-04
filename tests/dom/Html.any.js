const document = new Text().ownerDocument;

test(() => {
  const div = document.createElement('div');
  div.innerHTML = '<p class="a">one <b>two</b></p><!--c--><br>text &amp; &lt;';
  assert_equals(div.childNodes.length, 4);
  assert_equals(div.firstChild.localName, 'p');
  assert_equals(div.firstChild.className, 'a');
  assert_equals(div.firstChild.lastChild.textContent, 'two');
  assert_equals(div.childNodes[1].nodeType, Node.COMMENT_NODE);
  assert_equals(div.lastChild.data, 'text & <');
  assert_equals(div.innerHTML, '<p class="a">one <b>two</b></p><!--c--><br>text &amp; &lt;');
  div.innerHTML = '';
  assert_equals(div.childNodes.length, 0);
  div.innerHTML = null;
  assert_equals(div.innerHTML, '');
}, 'innerHTML parses and serializes');

test(() => {
  const table = document.createElement('table');
  table.innerHTML = '<tr><td>cell';
  assert_equals(table.firstChild.localName, 'tbody', 'the context decides how the fragment is parsed');
  const select = document.createElement('select');
  select.innerHTML = '<option>a<option>b';
  assert_equals(select.children.length, 2);
  const template = document.createElement('template');
  template.innerHTML = '<span>in</span>';
  assert_equals(template.childNodes.length, 0);
  assert_equals(template.innerHTML, '<span>in</span>');
  const textarea = document.createElement('textarea');
  textarea.innerHTML = '<b>not markup</b>';
  assert_equals(textarea.firstChild.data, '<b>not markup</b>');
  assert_equals(textarea.innerHTML, '&lt;b&gt;not markup&lt;/b&gt;', 'its text is escaped when read back');
}, 'the context element');

test(() => {
  const div = document.createElement('div');
  div.innerHTML = '<span id="x" title="a&quot;b">t</span>';
  const span = div.firstChild;
  assert_equals(span.outerHTML, '<span id="x" title="a&quot;b">t</span>');
  span.outerHTML = '<i>1</i><u>2</u>';
  assert_equals(div.innerHTML, '<i>1</i><u>2</u>');
  assert_equals(span.parentNode, null);
  assert_equals(document.createElement('p').outerHTML, '<p></p>');
  assert_equals(document.createElement('br').outerHTML, '<br>');
  const lone = document.createElement('p');
  lone.outerHTML = '<b>ignored</b>';
  assert_equals(lone.outerHTML, '<p></p>');
}, 'outerHTML');

test(() => {
  const div = document.createElement('div');
  div.innerHTML = '<b>mid</b>';
  const b = div.firstChild;
  b.insertAdjacentHTML('beforebegin', '<i>before</i>');
  b.insertAdjacentHTML('afterbegin', 'in-');
  b.insertAdjacentHTML('beforeend', '-out');
  b.insertAdjacentHTML('AFTEREND', '<u>after</u>');
  assert_equals(div.innerHTML, '<i>before</i><b>in-mid-out</b><u>after</u>');
  assert_throws_dom('SyntaxError', () => b.insertAdjacentHTML('inside', 'x'));
  const orphan = document.createElement('i');
  assert_throws_dom('NoModificationAllowedError', () => orphan.insertAdjacentHTML('beforebegin', 'x'));
}, 'insertAdjacentHTML');

test(() => {
  const parsed = new DOMParser().parseFromString('<!DOCTYPE html><title>T</title><p>Body', 'text/html');
  assert_equals(parsed.contentType, 'text/html');
  assert_equals(parsed.doctype.name, 'html');
  assert_equals(parsed.documentElement.localName, 'html');
  assert_equals(parsed.documentElement.firstChild.localName, 'head');
  assert_equals(parsed.documentElement.lastChild.firstChild.textContent, 'Body');
  assert_equals(parsed.getElementsByTagName('title')[0].textContent, 'T');
  assert_equals(parsed.compatMode, 'CSS1Compat');
  assert_equals(new DOMParser().parseFromString('<p>x', 'text/html').compatMode, 'BackCompat');
  assert_throws_dom('NotSupportedError', () => new DOMParser().parseFromString('<a/>', 'application/xml'));
  assert_throws_js(TypeError, () => new DOMParser().parseFromString('<a/>', 'text/plain'));
}, 'DOMParser');

test(() => {
  const div = document.createElement('div');
  div.innerHTML = '<svg viewBox="0 0 1 1"><foreignObject><p>in</p></foreignObject><path d="M0"/></svg>';
  const svg = div.firstChild;
  assert_equals(svg.namespaceURI, 'http://www.w3.org/2000/svg');
  assert_true(svg.hasAttribute('viewBox'), 'the case of an SVG attribute is fixed');
  assert_equals(svg.firstChild.localName, 'foreignObject');
  assert_equals(svg.firstChild.firstChild.namespaceURI, 'http://www.w3.org/1999/xhtml');
  assert_equals(svg.lastChild.localName, 'path');
  assert_equals(div.innerHTML, '<svg viewBox="0 0 1 1"><foreignObject><p>in</p></foreignObject><path d="M0"></path></svg>');
}, 'foreign content');
