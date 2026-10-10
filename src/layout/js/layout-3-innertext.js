// innerText and outerText (https://html.spec.whatwg.org/multipage/dom.html#the-innertext-idl-attribute): the text of an element as it is rendered, and the setters
// that make text and <br> of a string.
(function () {
  'use strict';
  const global = globalThis;
  const shared = global.__solarLayoutShared2;
  delete global.__solarLayoutShared2;
  const processedText = shared.processedText;
  const computed = (el) => global.getComputedStyle(el);
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { enumerable: true, configurable: true, ...descriptor });

  const BLOCK_LEVEL = new Set(['block', 'flow-root', 'list-item', 'table', 'flex', 'grid', 'table-caption', 'ruby-base-container']);

  // The items of the rendered text collection steps: strings, and numbers for the required line breaks.
  const collect = (node, isRoot) => {
    if (node.nodeType === 3) {
      const parent = node.parentElement;
      if (!parent) return [];
      const cs = computed(parent);
      if (cs.visibility !== 'visible') return [];
      const text = processedText(node);
      if (text === null) return [];
      return [{ text, collapsible: cs.whiteSpaceCollapse === 'collapse' || cs.whiteSpaceCollapse === 'preserve-breaks' }];
    }
    if (node.nodeType !== 1) return [];
    const cs = computed(node);
    if (cs.display === 'none') return [];
    const name = node.localName;
    // Content a control keeps to itself.
    if (name === 'textarea' || name === 'audio' || name === 'script' || name === 'style' || name === 'template') return [];
    let items = [];
    for (let child = node.firstChild; child; child = child.nextSibling) {
      if (name === 'select' && !(child.nodeType === 1 && (child.localName === 'option' || child.localName === 'optgroup'))) continue;
      if (name === 'details' && !node.open && !(child.nodeType === 1 && child.localName === 'summary')) continue;
      items = items.concat(collect(child, false));
    }
    if (cs.display === 'contents') return items;
    if (cs.visibility !== 'visible') return items;
    // An inline box that is a formatting context of its own does not pass white space out.
    if (cs.display === 'inline-block' || cs.display === 'inline-flex' || cs.display === 'inline-grid' || cs.display === 'inline-table') {
      for (let i = items.length - 1; i >= 0; i--) {
        if (typeof items[i] === 'object' && items[i].text !== '\n') {
          if (items[i].collapsible) items[i] = { ...items[i], text: items[i].text.replace(/ +$/, '') };
          break;
        }
        if (typeof items[i] === 'number') break;
      }
    }
    if (name === 'br') items.push({ text: '\n', collapsible: false });
    if (!isRoot) {
      if (cs.display === 'table-cell') {
        let last = true;
        for (let sibling = node.nextElementSibling; sibling; sibling = sibling.nextElementSibling) if (computed(sibling).display === 'table-cell') last = false;
        if (!last) items.push({ text: '\t', collapsible: false });
      } else if (cs.display === 'table-row') {
        let last = true;
        for (let sibling = node.nextElementSibling; sibling; sibling = sibling.nextElementSibling) if (computed(sibling).display === 'table-row') last = false;
        if (!last) items.push({ text: '\n', collapsible: false });
      }
    }
    if (name === 'p') {
      items.unshift(2);
      items.push(2);
    }
    // Block-level: what the display says, and what floats, positioning and being an item of a flex or grid container make of it.
    const parentDisplay = node.parentElement ? computed(node.parentElement).display : '';
    const blockified = cs.float !== 'none' || cs.position === 'absolute' || cs.position === 'fixed' || /flex|grid/.test(parentDisplay);
    if (BLOCK_LEVEL.has(cs.display) || (blockified && !isRoot)) {
      items.unshift(1);
      items.push(1);
    }
    return items;
  };

  const beingRendered = (el) => {
    const display = computed(el).display;
    if (display === 'none') return false;
    // Inside something with no box.
    for (let p = el.parentElement; p; p = p.parentElement) if (computed(p).display === 'none') return false;
    return el.isConnected && (display === 'contents' || el.getClientRects().length > 0);
  };

  const getInnerText = function (el) {
    if (!beingRendered(el)) return el.textContent;
    const items = collect(el, true);
    // Trailing white space of a line goes, and the white space that follows a line break.
    const out = [];
    let afterBreak = true;
    for (const item of items) {
      if (typeof item === 'number') {
        const last = out[out.length - 1];
        if (last && typeof last === 'object' && last.collapsible) last.text = last.text.replace(/ +$/, '');
        if (typeof last === 'number') out[out.length - 1] = Math.max(last, item);
        else out.push(item);
        afterBreak = true;
        continue;
      }
      let text = item.text;
      if (item.text === '\n') {
        const last = out[out.length - 1];
        if (last && typeof last === 'object' && last.collapsible) last.text = last.text.replace(/ +$/, '');
        out.push({ text, collapsible: false });
        afterBreak = true;
        continue;
      }
      if (afterBreak && item.collapsible) text = text.replace(/^ +/, '');
      if (text === '') continue;
      afterBreak = false;
      out.push({ text, collapsible: item.collapsible });
    }
    // The required line breaks at the start and the end are dropped; trailing white space of the last line goes.
    while (out.length && typeof out[0] === 'number') out.shift();
    while (out.length && typeof out[out.length - 1] === 'number') out.pop();
    let last = out[out.length - 1];
    if (last && typeof last === 'object' && last.collapsible) last.text = last.text.replace(/ +$/, '');
    let result = '';
    for (const item of out) result += typeof item === 'number' ? '\n'.repeat(item) : item.text;
    return result;
  };

  const fragmentFor = (doc, text) => {
    const fragment = doc.createDocumentFragment();
    let run = '';
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (c === '\n' || c === '\r') {
        if (run) fragment.appendChild(doc.createTextNode(run));
        run = '';
        if (c === '\r' && text[i + 1] === '\n') i++;
        fragment.appendChild(doc.createElement('br'));
      } else {
        run += c;
      }
    }
    if (run) fragment.appendChild(doc.createTextNode(run));
    return fragment;
  };
  const toText = (value) => (value === null ? '' : String(value));

  const innerDescriptor = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'innerText');
  define(HTMLElement.prototype, 'innerText', {
    get() { return getInnerText(this); },
    set(value) {
      const text = toText(value);
      this.replaceChildren(fragmentFor(this.ownerDocument, text));
    },
  });
  void innerDescriptor;
  define(HTMLElement.prototype, 'outerText', {
    get() { return getInnerText(this); },
    set(value) {
      const text = toText(value);
      const parent = this.parentNode;
      if (!parent) throw new DOMException("Failed to set the 'outerText' property on 'HTMLElement': The element has no parent node.", 'NoModificationAllowedError');
      const next = this.nextSibling, previous = this.previousSibling;
      const fragment = fragmentFor(this.ownerDocument, text);
      parent.replaceChild(fragment, this);
      // Text on either side is joined with the new text.
      if (next && next.previousSibling && next.previousSibling.nodeType === 3 && next.nodeType === 3) {
        const left = next.previousSibling;
        next.data = left.data + next.data;
        left.remove();
      }
      if (previous && previous.nodeType === 3 && previous.nextSibling && previous.nextSibling.nodeType === 3) {
        const right = previous.nextSibling;
        previous.data += right.data;
        right.remove();
      }
    },
  });
})();
