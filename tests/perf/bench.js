// Performance benchmarks of the page machinery: DOM, style, layout, animations, as a script on the page does them. Each prints one line "BENCH <name> <ms per operation> <operations>".
// Run them with tests/perf/run.py (a window-less Solar loads bench.html and the lines are read from what it prints).
(function () {
  const results = [];
  const now = () => performance.now();
  const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
  // time(name, op, {ops, runs, setup}): op() is run `runs` times after setup() each time; the median of the time of a run, divided by `ops` (what one run does that many times).
  const only = (location.search.match(/only=([^&]+)/) || [0, ''])[1];
  const onlyRe = only ? new RegExp(decodeURIComponent(only)) : null;
  const verbose = /verbose=1/.test(location.search);
  const extraRuns = Number((location.search.match(/runs=(\d+)/) || [0, 0])[1]);
  const time = (name, op, options = {}) => {
    if (onlyRe && !onlyRe.test(name)) return;
    const runs = extraRuns || options.runs || 5, ops = options.ops || 1;
    const samples = [];
    for (let r = 0; r < runs; r++) {
      if (options.setup) options.setup();
      document.body.offsetHeight;  // (a clean tree before the run)
      const t0 = now();
      op();
      samples.push(now() - t0);
      if (verbose) console.log('SAMPLE ' + name + ' ' + (now() - t0).toFixed(1));
    }
    const ms = median(samples) / ops;
    results.push({ name, ms, ops });
    console.log('BENCH ' + name + ' ' + ms.toFixed(4) + ' ' + ops);
  };

  // ---- A page of the kind there are: a header, a list of cards, a table, a form, with a style sheet of a few hundred rules, some of them in @media and @container ----
  const make = (cards) => {
    const css = [];
    for (let i = 0; i < 300; i++) css.push(`.c${i % 40} .t${i % 17} { color: rgb(${i % 255}, 10, 20); margin: ${i % 7}px; }`);
    const mode = (location.search.match(/layout=(\w+)/) || [0, 'flex'])[1];  // how the cards are put in a row: flex (wrap), grid, float, block (one under the other)
    const place = { flex: '', float: 'float: left;', grid: '', block: '' }[mode];
    css.push('#cards { ' + ({ flex: 'display: flex; flex-wrap: wrap;', grid: 'display: grid; grid-template-columns: repeat(auto-fill, minmax(290px, 1fr));', float: '', block: '' }[mode]) + ' }');
    css.push('.card { display: flex; flex-direction: column; padding: 8px; border: 1px solid #ccc; margin: 4px; ' + (mode === 'grid' || mode === 'block' ? '' : 'width: 280px; ') + place + ' container-type: inline-size; }');
    css.push('.card h3 { font-size: 16px; margin: 0 0 4px; } .card p { margin: 0; color: #444; } .card .row { display: flex; justify-content: space-between; }');
    css.push('@container (min-width: 200px) { .card h3 { font-size: 18px; } .card .row { gap: 8px; } }');
    css.push('@media (min-width: 600px) { .card { width: 300px; } .nav a { padding: 6px 12px; } } @media (max-width: 300px) { .card { width: 100%; } }');
    css.push('.nav { display: flex; gap: 8px; } .nav a { text-decoration: none; color: #06c; } table { border-collapse: collapse; } td, th { border: 1px solid #ddd; padding: 2px 6px; }');
    const style = document.createElement('style');
    style.textContent = css.join('\n');
    document.head.appendChild(style);
    const root = document.createElement('div');
    root.id = 'root';
    let html = '<div class="nav">' + Array.from({ length: 20 }, (_, i) => `<a href="#${i}" class="t${i % 17}">Link ${i}</a>`).join('') + '</div><div id="cards">';
    for (let i = 0; i < cards; i++) {
      html += `<div class="card c${i % 40}" data-i="${i}"><h3 class="t${i % 17}">Card ${i} title</h3><p>Some text of the card number ${i}, long enough to wrap in a column of this width, and a little more.</p><div class="row"><span>price ${i}</span><button type="button">Buy</button></div></div>`;
    }
    html += '</div><div style="clear:both"></div><table id="table">' + Array.from({ length: 60 }, (_, r) => '<tr>' + Array.from({ length: 12 }, (_, c) => `<td>${r}.${c}</td>`).join('') + '</tr>').join('') + '</table>';
    root.innerHTML = html;
    document.body.appendChild(root);
    return root;
  };
  const reset = () => { const old = document.getElementById('root'); if (old) old.remove(); for (const s of document.head.querySelectorAll('style')) s.remove(); };

  const run = (cards) => {
    reset();
    const t0 = now();
    const root = make(cards);
    document.body.offsetHeight;
    console.log('BENCH ' + cards + ': build page and first layout ' + (now() - t0).toFixed(2) + ' ' + 1);
    const label = cards + ': ';
    const elements = document.querySelectorAll('*').length;
    console.log('INFO ' + cards + ' cards, ' + elements + ' elements, layout ' + ((location.search.match(/layout=(\w+)/) || [0, 'flex'])[1]));
    const cardList = root.querySelectorAll('.card');
    const mid = cardList[Math.floor(cardList.length / 2)];
    const midText = mid.querySelector('p');

    // Reads on a clean tree: they cost nothing but the read.
    time(label + 'clientWidth, clean tree', () => { for (let i = 0; i < 1000; i++) mid.clientWidth; }, { ops: 1000 });
    time(label + 'getBoundingClientRect, clean tree', () => { for (let i = 0; i < 1000; i++) mid.getBoundingClientRect(); }, { ops: 1000 });
    time(label + 'getComputedStyle(el).color, clean tree', () => { for (let i = 0; i < 1000; i++) getComputedStyle(mid).color; }, { ops: 1000 });
    time(label + 'getComputedStyle().width on 200 cards, clean', () => { for (const c of [...cardList].slice(0, 200)) getComputedStyle(c).width; }, { ops: 200 });

    // A change, then a read: the price of a forced layout after a small change.
    time(label + 'text change in one card, then clientWidth', () => { midText.firstChild.data += '.'; mid.clientWidth; }, { runs: 7 });
    time(label + 'shift everything by 1px (padding-top of #root), then clientWidth', () => { root.style.paddingTop = (root.style.paddingTop === '1px' ? '0px' : '1px'); mid.clientWidth; }, { runs: 7 });
    time(label + 'attribute (data-x) change, then clientWidth', () => { mid.setAttribute('data-x', String(Math.random())); mid.clientWidth; }, { runs: 7 });
    time(label + 'class toggle on one card, then clientWidth', () => { mid.classList.toggle('hot'); mid.clientWidth; }, { runs: 7 });
    time(label + 'style.width change on one card, then clientWidth', () => { mid.style.width = (250 + Math.floor(Math.random() * 40)) + 'px'; mid.clientWidth; }, { runs: 7 });
    time(label + 'class toggle on #root (everything below), then clientWidth', () => { root.classList.toggle('mode'); mid.clientWidth; }, { runs: 7 });
    time(label + 'append one card, then clientWidth', () => { const c = mid.cloneNode(true); mid.parentNode.appendChild(c); mid.clientWidth; c.remove(); }, { runs: 7 });
    time(label + 'remove and reinsert one card, then clientWidth', () => { const p = mid.parentNode, n = mid.nextSibling; mid.remove(); p.insertBefore(mid, n); mid.clientWidth; }, { runs: 7 });
    time(label + 'window.scrollTo + scrollY read', () => { window.scrollTo(0, 100 + Math.floor(Math.random() * 50)); window.scrollY; }, { runs: 7 });

    // Many changes between reads (what a script that builds does), and a read after each (layout thrash).
    time(label + '100 cards: change text, read after each (thrash)', () => { for (let i = 0; i < 100; i++) { cardList[i].querySelector('span').textContent = 'p' + i + Math.random(); cardList[i].clientHeight; } }, { ops: 100, runs: 3 });
    time(label + '100 cards: change text, one read at the end', () => { for (let i = 0; i < 100; i++) cardList[i].querySelector('span').textContent = 'p' + i + Math.random(); document.body.offsetHeight; }, { ops: 100, runs: 3 });
    time(label + 'set 1000 style properties, then read', () => { for (let i = 0; i < 1000; i++) cardList[i % cardList.length].style.marginLeft = (i % 9) + 'px'; document.body.offsetHeight; }, { ops: 1000, runs: 3 });
    time(label + 'toggle a class on 200 cards, then read', () => { for (let i = 0; i < 200; i++) cardList[i % cardList.length].classList.toggle('hot'); document.body.offsetHeight; }, { ops: 200, runs: 3 });

    // The DOM alone, no style or layout read.
    time(label + 'createElement + appendChild x1000 (detached)', () => { const f = document.createElement('div'); for (let i = 0; i < 1000; i++) { const e = document.createElement('span'); e.textContent = 'x'; f.appendChild(e); } }, { ops: 1000 });
    time(label + 'innerHTML of 200 cards (parse), no read', () => { const d = document.createElement('div'); d.innerHTML = root.querySelector('#cards').innerHTML; }, { ops: 1, runs: 3 });
    time(label + 'innerHTML into the page, then layout', () => { const d = document.createElement('div'); d.innerHTML = '<div class="card">' + 'x'.repeat(50) + '</div>'.repeat(1); root.appendChild(d); document.body.offsetHeight; d.remove(); }, { runs: 5 });
    time(label + 'querySelectorAll(".card .t3")', () => { for (let i = 0; i < 20; i++) root.querySelectorAll('.card .t3'); }, { ops: 20 });
    time(label + 'querySelectorAll("[data-i]")', () => { for (let i = 0; i < 20; i++) root.querySelectorAll('[data-i]'); }, { ops: 20 });
    time(label + 'matches(".card:nth-child(3n) h3") x1000', () => { const h = mid.querySelector('h3'); for (let i = 0; i < 1000; i++) h.matches('.card:nth-child(3n) h3'); }, { ops: 1000 });
    time(label + 'closest(".card") x1000', () => { const h = mid.querySelector('span'); for (let i = 0; i < 1000; i++) h.closest('.card'); }, { ops: 1000 });
    time(label + 'cloneNode(true) of a card x200', () => { for (let i = 0; i < 200; i++) mid.cloneNode(true); }, { ops: 200 });
    time(label + 'textContent read of #root', () => { for (let i = 0; i < 20; i++) root.textContent; }, { ops: 20 });
    time(label + 'dispatchEvent x5000 (one listener)', () => { const f = () => {}; mid.addEventListener('ping', f); for (let i = 0; i < 5000; i++) mid.dispatchEvent(new Event('ping', { bubbles: true })); mid.removeEventListener('ping', f); }, { ops: 5000 });
    time(label + 'MutationObserver: 500 changes, callbacks', () => { let n = 0; const mo = new MutationObserver((l) => { n += l.length; }); mo.observe(root, { subtree: true, attributes: true, childList: true, characterData: true }); for (let i = 0; i < 500; i++) cardList[i % cardList.length].setAttribute('data-m', String(i)); mo.takeRecords(); mo.disconnect(); }, { ops: 500, runs: 3 });
    time(label + 'IntersectionObserver: observe 200 cards, one frame', () => { const io = new IntersectionObserver(() => {}); for (const c of [...cardList].slice(0, 200)) io.observe(c); io.disconnect(); }, { ops: 200, runs: 3 });
    time(label + 'getElementsByClassName x100', () => { for (let i = 0; i < 100; i++) root.getElementsByClassName('card').length; }, { ops: 100 });
    time(label + 'createTreeWalker over #root', () => { const w = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT); let n = 0; while (w.nextNode()) n++; }, { ops: 1 });
    time(label + 'Range.getBoundingClientRect on a text', () => { const r = document.createRange(); r.selectNodeContents(midText); for (let i = 0; i < 200; i++) r.getBoundingClientRect(); }, { ops: 200 });
    time(label + 'attachShadow + 100 shadow children x20', () => { for (let i = 0; i < 20; i++) { const h = document.createElement('div'); const s = h.attachShadow({ mode: 'open' }); for (let j = 0; j < 100; j++) s.appendChild(document.createElement('i')); } }, { ops: 20, runs: 3 });
    time(label + 'customElements: define once, create 300', () => { const name = 'x-b' + Math.floor(Math.random() * 1e9); customElements.define(name, class extends HTMLElement { connectedCallback() { this.textContent = 'c'; } }); for (let i = 0; i < 300; i++) root.appendChild(document.createElement(name)); document.body.offsetHeight; for (const e of root.querySelectorAll('[class=""]')) e.remove(); }, { ops: 300, runs: 2 });
  };

  // ---- Layout of the shapes of a page ----
  const shapes = () => {
    reset();
    const mk = (tag, css, n, inner) => { const d = document.createElement(tag); d.style.cssText = css; for (let i = 0; i < n; i++) { const c = document.createElement('div'); c.textContent = inner(i); c.style.cssText = 'padding:2px;border:1px solid #888'; d.appendChild(c); } document.body.appendChild(d); return d; };
    const timeShape = (name, css, n, tag = 'div') => {
      time('shape: ' + name + ' x' + n + ' first layout', () => { const d = mk(tag, css, n, (i) => 'item ' + i + ' with some words'); document.body.offsetHeight; d.remove(); }, { runs: 3 });
    };
    timeShape('block flow', 'width:600px', 1000);
    timeShape('flex row wrap', 'display:flex;flex-wrap:wrap;width:600px', 1000);
    timeShape('grid auto-fill', 'display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));width:600px', 1000);
    timeShape('float left', 'width:600px', 1000);
    timeShape('inline-block', 'width:600px', 1000);
    const t0 = now();
    const tbl = document.createElement('table'); tbl.innerHTML = Array.from({ length: 100 }, (_, r) => '<tr>' + Array.from({ length: 20 }, (_, c) => `<td>${r}${c} cell text</td>`).join('') + '</tr>').join(''); document.body.appendChild(tbl); document.body.offsetHeight;
    console.log('BENCH shape: table 100x20 first layout ' + (now() - t0).toFixed(2) + ' 1');
    time('shape: table, one cell text change + read', () => { tbl.rows[50].cells[10].textContent += 'x'; tbl.clientWidth; }, { runs: 5 });
    tbl.remove();
    const p = document.createElement('p'); p.style.width = '400px'; p.textContent = 'lorem ipsum dolor sit amet '.repeat(2000); document.body.appendChild(p); document.body.offsetHeight;
    time('shape: one paragraph of 10000 words, width change + read', () => { p.style.width = (300 + Math.floor(Math.random() * 200)) + 'px'; p.clientHeight; }, { runs: 5 });
    time('shape: one paragraph, appendData of a word + read', () => { p.firstChild.appendData(' more'); p.clientHeight; }, { runs: 5 });
    p.remove();
  };

  // ---- An animation of sizes on a page with container queries, and a script that reads sizes while it runs (a carousel of the kind there are) ----
  const animated = (cards) => {
    reset();
    const root = make(cards);
    const st = document.createElement('style');
    st.textContent = '@keyframes grow { from { min-width: 100px; } to { min-width: 140px; } } #grow { animation: grow 2s linear infinite alternate; background: #eee; height: 20px; }';
    document.head.appendChild(st);
    const g = document.createElement('div'); g.id = 'grow'; root.prepend(g);
    document.body.offsetHeight;
    const cardList = root.querySelectorAll('.card');
    return new Promise((resolve) => {
      let frames = 0; const t0 = now(); const samples = [];
      const tick = () => {
        const a = now();
        cardList[frames % cardList.length].querySelector('span').textContent = 'f' + frames;  // a change of the page
        document.body.offsetHeight;                                                            // a read of a size after it
        samples.push(now() - a);
        if (++frames < 60) requestAnimationFrame(tick); else { const ms = median(samples); console.log('BENCH ' + cards + ': animated min-width + change + read per frame ' + ms.toFixed(3) + ' 60'); resolve(); }
      };
      requestAnimationFrame(tick);
    });
  };

  const main = async () => {
    const sizes = (location.search.match(/cards=([\d,]+)/) || [0, '200'])[1].split(',').map(Number);
    for (const n of sizes) run(n);
    shapes();
    for (const n of sizes) await animated(n);
    console.log('BENCHDONE');
  };
  window.addEventListener('load', () => setTimeout(main, 100));
})();
