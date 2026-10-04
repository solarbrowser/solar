// The document tree through its JavaScript interface.
// The page has no window yet; the document the constructors use is the one the realm has.
const document = new Text().ownerDocument;

test(() => {
  const doc = new Document();
  assert_true(doc instanceof Node);
  assert_true(doc instanceof EventTarget);
  assert_equals(doc.nodeType, Node.DOCUMENT_NODE);
  assert_equals(doc.nodeName, '#document');
  assert_equals(doc.contentType, 'application/xml');
  assert_equals(doc.URL, 'about:blank');
  assert_equals(doc.compatMode, 'CSS1Compat');
  assert_equals(doc.doctype, null);
  assert_equals(doc.documentElement, null);
  assert_equals(doc.ownerDocument, null);
}, 'a new Document');

test(() => {
  const doc = document.implementation.createHTMLDocument('title');
  assert_equals(doc.contentType, 'text/html');
  assert_equals(doc.doctype.name, 'html');
  assert_equals(doc.documentElement.localName, 'html');
  assert_equals(doc.documentElement.firstChild.localName, 'head');
  assert_equals(doc.documentElement.firstChild.firstChild.textContent, 'title');
  assert_equals(doc.documentElement.lastChild.localName, 'body');
}, 'createHTMLDocument');

test(() => {
  const div = document.createElement('DIV');
  assert_true(div instanceof Element);
  assert_true(div instanceof HTMLElement);
  assert_equals(div.localName, 'div');
  assert_equals(div.tagName, 'DIV');
  assert_equals(div.nodeName, 'DIV');
  assert_equals(div.namespaceURI, 'http://www.w3.org/1999/xhtml');
  assert_equals(div.prefix, null);
  assert_equals(div.ownerDocument, document);
  assert_equals(div.parentNode, null);
  assert_false(div.isConnected);
  assert_throws_dom('InvalidCharacterError', () => document.createElement('1a'));
  assert_throws_dom('InvalidCharacterError', () => document.createElement(''));
}, 'createElement');

test(() => {
  const el = document.createElementNS('http://www.w3.org/2000/svg', 'svg:rect');
  assert_equals(el.prefix, 'svg');
  assert_equals(el.localName, 'rect');
  assert_equals(el.tagName, 'svg:rect');
  assert_not_equals(el.constructor, HTMLElement);
  assert_throws_dom('NamespaceError', () => document.createElementNS(null, 'a:b'));
  assert_throws_dom('NamespaceError', () => document.createElementNS('http://example.com', 'xml:b'));
  assert_throws_dom('NamespaceError', () => document.createElementNS('http://www.w3.org/2000/xmlns/', 'a'));
  assert_throws_dom('InvalidCharacterError', () => document.createElementNS('x', 'a:b:c'));
}, 'createElementNS');

test(() => {
  const parent = document.createElement('div');
  const a = document.createElement('a');
  const b = document.createElement('b');
  assert_equals(parent.appendChild(a), a);
  assert_equals(parent.insertBefore(b, a), b);
  assert_equals(parent.firstChild, b);
  assert_equals(parent.lastChild, a);
  assert_equals(b.nextSibling, a);
  assert_equals(a.previousSibling, b);
  assert_equals(parent.childNodes.length, 2);
  assert_equals(parent.childNodes.item(0), b);
  assert_equals(parent.childNodes, parent.childNodes, 'the same object each time');
  assert_equals(parent.children.length, 2);
  assert_equals(parent.childElementCount, 2);
  assert_equals(parent.firstElementChild, b);
  assert_equals(b.nextElementSibling, a);
  assert_equals(a.previousElementSibling, b);
  assert_equals(a.parentElement, parent);
  assert_equals(parent.replaceChild(b, a), a);
  assert_equals(parent.childNodes.length, 1);
  assert_equals(parent.removeChild(b), b);
  assert_false(parent.hasChildNodes());
  assert_equals(parent.childNodes.length, 0, 'a live list');
}, 'the tree operations');

test(() => {
  const parent = document.createElement('div');
  assert_throws_dom('HierarchyRequestError', () => parent.appendChild(parent));
  assert_throws_dom('NotFoundError', () => parent.removeChild(document.createElement('p')));
  assert_throws_dom('NotFoundError', () => parent.insertBefore(document.createElement('p'), document.createElement('q')));
  assert_throws_js(TypeError, () => parent.appendChild({}));
  assert_throws_js(TypeError, () => parent.appendChild());
  assert_throws_dom('HierarchyRequestError', () => document.appendChild(document.createTextNode('x')));
}, 'what the tree refuses');

test(() => {
  const parent = document.createElement('div');
  parent.append('a', document.createElement('b'), 'c');
  assert_equals(parent.childNodes.length, 3);
  assert_equals(parent.textContent, 'ac');
  parent.prepend('x');
  assert_equals(parent.firstChild.data, 'x');
  parent.firstElementChild.before('before');
  parent.firstElementChild.after('after');
  assert_equals(parent.textContent, 'xabeforeafterc');
  parent.firstElementChild.replaceWith('r', document.createElement('i'));
  assert_equals(parent.firstElementChild.localName, 'i');
  parent.replaceChildren('only');
  assert_equals(parent.childNodes.length, 1);
  parent.firstChild.remove();
  assert_false(parent.hasChildNodes());
  parent.replaceChildren();
  const fragment = document.createDocumentFragment();
  fragment.append(document.createElement('p'), document.createElement('q'));
  parent.append(fragment);
  assert_equals(parent.children.length, 2);
  assert_equals(fragment.childNodes.length, 0);
}, 'the ParentNode and ChildNode mixins');

test(() => {
  const el = document.createElement('div');
  assert_false(el.hasAttributes());
  el.setAttribute('ID', 'x');
  assert_equals(el.id, 'x');
  assert_equals(el.getAttribute('id'), 'x');
  assert_equals(el.getAttribute('missing'), null);
  assert_true(el.hasAttribute('Id'));
  el.className = 'a b';
  assert_equals(el.getAttribute('class'), 'a b');
  assert_array_equals(el.getAttributeNames(), ['id', 'class']);
  assert_true(el.toggleAttribute('hidden'));
  assert_false(el.toggleAttribute('hidden'));
  assert_true(el.toggleAttribute('hidden', true));
  assert_true(el.toggleAttribute('hidden', true));
  el.removeAttribute('hidden');
  assert_false(el.hasAttribute('hidden'));
  assert_throws_dom('InvalidCharacterError', () => el.setAttribute('a b', ''));
  el.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', 'u');
  assert_equals(el.getAttributeNS('http://www.w3.org/1999/xlink', 'href'), 'u');
  assert_equals(el.getAttribute('xlink:href'), 'u');
  const attr = el.getAttributeNode('id');
  assert_true(attr instanceof Attr);
  assert_equals(attr.name, 'id');
  assert_equals(attr.value, 'x');
  assert_equals(attr.ownerElement, el);
  attr.value = 'y';
  assert_equals(el.id, 'y');
  const other = document.createElement('p');
  assert_throws_dom('InUseAttributeError', () => other.setAttributeNode(attr));
  assert_equals(el.removeAttributeNode(attr), attr);
  assert_equals(attr.ownerElement, null);
  assert_false(el.hasAttribute('id'));
  assert_throws_dom('NotFoundError', () => el.removeAttributeNode(attr));
}, 'attributes');

test(() => {
  const text = document.createTextNode('hello world');
  assert_equals(text.data, 'hello world');
  assert_equals(text.length, 11);
  assert_equals(text.substringData(6, 5), 'world');
  text.appendData('!');
  text.insertData(0, '>> ');
  assert_equals(text.data, '>> hello world!');
  text.deleteData(0, 3);
  text.replaceData(0, 5, 'HELLO');
  assert_equals(text.data, 'HELLO world!');
  assert_throws_dom('IndexSizeError', () => text.substringData(100, 1));
  const parent = document.createElement('p');
  parent.appendChild(text);
  const tail = text.splitText(5);
  assert_equals(text.data, 'HELLO');
  assert_equals(tail.data, ' world!');
  assert_equals(text.nextSibling, tail);
  assert_equals(text.wholeText, 'HELLO world!');
  assert_equals(parent.textContent, 'HELLO world!');
  parent.normalize();
  assert_equals(parent.childNodes.length, 1);
  assert_equals(parent.firstChild.data, 'HELLO world!');
  const emoji = document.createTextNode('a\u{1F600}b');
  assert_equals(emoji.length, 4, 'length counts UTF-16 code units');
  assert_equals(emoji.substringData(1, 2), '\u{1F600}');
}, 'character data');

test(() => {
  const root = document.createElement('ul');
  root.setAttribute('class', 'list');
  root.innerHTML === undefined;
  for (const text of ['one', 'two']) {
    const li = document.createElement('li');
    li.textContent = text;
    root.append(li);
  }
  const deep = root.cloneNode(true);
  const shallow = root.cloneNode();
  assert_not_equals(deep, root);
  assert_true(deep.isEqualNode(root));
  assert_false(deep.isSameNode(root));
  assert_true(root.isSameNode(root));
  assert_equals(deep.childNodes.length, 2);
  assert_equals(shallow.childNodes.length, 0);
  assert_false(shallow.isEqualNode(root));
  assert_equals(deep.getAttribute('class'), 'list');
  deep.setAttribute('class', 'other');
  assert_equals(root.getAttribute('class'), 'list');
  assert_false(root.isEqualNode(null));
}, 'cloneNode and isEqualNode');

test(() => {
  const a = document.createElement('a');
  const b = document.createElement('b');
  const c = document.createElement('c');
  a.append(b, c);
  assert_equals(b.compareDocumentPosition(c), Node.DOCUMENT_POSITION_FOLLOWING);
  assert_equals(c.compareDocumentPosition(b), Node.DOCUMENT_POSITION_PRECEDING);
  assert_equals(b.compareDocumentPosition(a), Node.DOCUMENT_POSITION_CONTAINS | Node.DOCUMENT_POSITION_PRECEDING);
  assert_equals(a.compareDocumentPosition(b), Node.DOCUMENT_POSITION_CONTAINED_BY | Node.DOCUMENT_POSITION_FOLLOWING);
  assert_equals(a.compareDocumentPosition(a), 0);
  assert_true(a.contains(b));
  assert_true(a.contains(a));
  assert_false(b.contains(a));
  assert_false(a.contains(null));
  assert_true(!!(a.compareDocumentPosition(document.createElement('x')) & Node.DOCUMENT_POSITION_DISCONNECTED));
  assert_equals(c.getRootNode(), a);
}, 'document position and containment');

test(() => {
  const doc = document.implementation.createDocument('http://www.w3.org/2000/svg', 'svg', null);
  assert_equals(doc.contentType, 'image/svg+xml');
  assert_equals(doc.documentElement.namespaceURI, 'http://www.w3.org/2000/svg');
  assert_equals(doc.documentElement.lookupNamespaceURI(null), 'http://www.w3.org/2000/svg');
  assert_equals(doc.lookupNamespaceURI(null), 'http://www.w3.org/2000/svg');
  assert_equals(doc.lookupPrefix('http://www.w3.org/2000/svg'), null);
  assert_true(doc.documentElement.isDefaultNamespace('http://www.w3.org/2000/svg'));
  assert_equals(doc.lookupNamespaceURI('xml'), 'http://www.w3.org/XML/1998/namespace');
  const text = doc.createCDATASection('x');
  assert_equals(text.nodeName, '#cdata-section');
  assert_throws_dom('InvalidCharacterError', () => doc.createCDATASection('a]]>b'));
  assert_throws_dom('NotSupportedError', () => document.createCDATASection('x'));
  const pi = doc.createProcessingInstruction('target', 'data');
  assert_equals(pi.target, 'target');
  assert_equals(pi.nodeName, 'target');
  assert_throws_dom('InvalidCharacterError', () => doc.createProcessingInstruction('t', 'a?>b'));
}, 'XML documents, namespaces and the other node types');

test(() => {
  const other = document.implementation.createHTMLDocument('');
  const el = document.createElement('div');
  el.setAttribute('a', 'b');
  const imported = other.importNode(el, true);
  assert_equals(imported.ownerDocument, other);
  assert_equals(el.ownerDocument, document);
  const parent = document.createElement('p');
  const child = document.createElement('c');
  parent.append(child);
  assert_equals(other.adoptNode(parent), parent);
  assert_equals(parent.ownerDocument, other);
  assert_equals(child.ownerDocument, other);
  const body = other.documentElement.lastChild;
  body.appendChild(document.createElement('d'));
  assert_equals(body.firstChild.ownerDocument, other, 'inserting adopts');
  assert_throws_dom('NotSupportedError', () => other.importNode(document));
}, 'importNode and adoptNode');

test(() => {
  const root = document.createElement('div');
  root.innerHTML === undefined;
  const a = document.createElement('span');
  a.className = 'x y';
  a.id = 'first';
  const b = document.createElement('span');
  b.className = 'y';
  const inner = document.createElement('p');
  inner.append(b);
  root.append(a, inner);
  assert_equals(root.getElementsByTagName('span').length, 2);
  assert_equals(root.getElementsByTagName('SPAN').length, 2);
  assert_equals(root.getElementsByTagName('*').length, 3);
  assert_equals(root.getElementsByClassName('y').length, 2);
  assert_equals(root.getElementsByClassName('x y').length, 1);
  assert_equals(root.getElementsByClassName('').length, 0);
  const live = root.getElementsByTagName('span');
  assert_equals(live.length, 2);
  root.append(document.createElement('span'));
  assert_equals(live.length, 3, 'a live collection');
  assert_equals(live.item(0), a);
  assert_equals(live.item(10), null);
  assert_equals(live.namedItem('first'), a);
  assert_equals(live.namedItem(''), null);
  const detached = document.createElement('div');
  detached.append(root);
  assert_equals(detached.getElementsByTagName('span').length, 3);
  assert_equals(document.getElementById('nothing'), null);
  document.documentElement && 0;
}, 'collections');

test(() => {
  const text = document.createTextNode('t');
  const comment = document.createComment('c');
  assert_equals(text.nodeValue, 't');
  text.nodeValue = 'u';
  assert_equals(text.data, 'u');
  assert_equals(comment.nodeName, '#comment');
  const el = document.createElement('div');
  assert_equals(el.nodeValue, null);
  assert_equals(el.textContent, '');
  el.textContent = 'hi';
  assert_equals(el.firstChild.data, 'hi');
  el.textContent = '';
  assert_equals(el.firstChild, null);
  assert_equals(document.textContent, null);
  assert_equals(Node.ELEMENT_NODE, 1);
  assert_equals(el.DOCUMENT_POSITION_CONTAINS, 8);
  assert_throws_js(TypeError, () => Node.prototype.appendChild.call({}, el));
  assert_throws_js(TypeError, () => new Node());
}, 'node values and constants');

test(() => {
  const parent = document.createElement('div');
  const a = document.createElement('a');
  const b = document.createElement('b');
  b.id = 'bee';
  b.setAttribute('name', 'buzz');
  parent.append(a, 'text', b);
  const nodes = parent.childNodes;
  assert_equals(nodes[0], a);
  assert_equals(nodes[1].data, 'text');
  assert_equals(nodes[3], undefined);
  assert_equals(nodes.length, 3);
  assert_array_equals([...nodes].map((n) => n.nodeName), ['A', '#text', 'B']);
  assert_array_equals(Object.keys(nodes), ['0', '1', '2']);
  const seen = [];
  nodes.forEach((node, index, list) => { seen.push(index); assert_equals(list, nodes); });
  assert_array_equals(seen, [0, 1, 2]);
  assert_equals(NodeList.prototype[Symbol.iterator], NodeList.prototype.values);
  assert_throws_js(TypeError, () => { 'use strict'; nodes[0] = a; });

  const elements = parent.children;
  assert_equals(elements[1], b);
  assert_equals(elements['bee'], b);
  assert_equals(elements['buzz'], b);
  assert_equals(elements['nothing'], undefined);
  assert_array_equals(Object.keys(elements), ['0', '1']);
  assert_array_equals(Object.getOwnPropertyNames(elements), ['0', '1', 'bee', 'buzz']);
  assert_true('bee' in elements);
  assert_array_equals([...elements], [a, b]);
  parent.append(document.createElement('c'));
  assert_equals(elements.length, 3, 'live');
  assert_equals(elements[2].localName, 'c');
}, 'indexed and named access on the collections');

test(() => {
  const text = document.createTextNode('a\uD800b');
  assert_equals(text.data, 'a\uD800b', 'a lone surrogate is kept');
  assert_equals(text.length, 3);
  const other = document.createTextNode('\uDC00');
  other.insertData(0, '\uD800');
  assert_equals(other.data, '𐀀');
  assert_equals(other.length, 2);
  const split = document.createTextNode('😀');
  const parent = document.createElement('p');
  parent.append(split);
  const tail = split.splitText(1);
  assert_equals(split.data, '\uD83D');
  assert_equals(tail.data, '\uDE00');
  assert_equals(parent.textContent, '😀', 'rejoined in the string');
  assert_equals(split.substringData(0, 1), '\uD83D');
}, 'character data keeps lone surrogates');
