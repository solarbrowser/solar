// Style-heavy benchmarks on a page of the kind there are: a framework-sized style sheet (resets, utility classes, BEM components, descendant / child / :not / :is / :has / attribute /
// nth-child selectors, CSS variables of a design system with a dark theme, @media, @supports, @layer), and a document of about 12000 elements (an article, an app with utility
// classes, a feed of components). Each prints one line "BENCH <name> <ms per operation> <operations>". Run: tests/perf/run.py --suite style
(function () {
  const now = () => performance.now();
  const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
  const only = (location.search.match(/only=([^&]+)/) || [0, ''])[1];
  const onlyRe = only ? new RegExp(decodeURIComponent(only)) : null;
  const extraRuns = Number((location.search.match(/runs=(\d+)/) || [0, 0])[1]);
  const time = (name, op, options = {}) => {
    if (onlyRe && !onlyRe.test(name)) return;
    const runs = extraRuns || options.runs || 5, ops = options.ops || 1, samples = [];
    for (let r = 0; r < runs; r++) {
      if (options.setup) options.setup();
      document.body.offsetHeight;
      const t0 = now(); op(); samples.push(now() - t0);
    }
    console.log('BENCH ' + name + ' ' + (median(samples) / ops).toFixed(4) + ' ' + ops);
  };
  let seed = 12345;
  const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  const pick = (a) => a[rnd(a.length)];

  // ---- The style sheet ----
  const sheet = () => {
    const css = [];
    css.push('@layer base, components, utilities;');
    css.push(':root { --fg: #222; --bg: #fff; --muted: #666; --accent: #0a58ca; --border: #ddd; --space: 4px; --space-1: var(--space); --space-2: calc(var(--space) * 2); --space-3: calc(var(--space) * 3); --space-4: calc(var(--space) * 4); --radius: 6px; --shadow: 0 1px 3px rgba(0,0,0,.2); --font: system-ui, sans-serif; --fs: 14px; --lh: 1.5; }');
    css.push(':root[data-theme="dark"] { --fg: #eee; --bg: #121212; --muted: #aaa; --accent: #6ea8fe; --border: #333; --shadow: 0 1px 3px rgba(0,0,0,.8); }');
    css.push('@layer base { *, *::before, *::after { box-sizing: border-box; } html { font: var(--fs)/var(--lh) var(--font); color: var(--fg); background: var(--bg); } body { margin: 0; } h1,h2,h3,h4,h5,h6 { margin: 0 0 var(--space-2); line-height: 1.2; } p { margin: 0 0 var(--space-3); } ul, ol { margin: 0; padding-left: 20px; } a { color: var(--accent); text-decoration: none; } a:hover { text-decoration: underline; } button, input, select, textarea { font: inherit; color: inherit; } table { border-collapse: collapse; } td, th { padding: var(--space-1) var(--space-2); border-bottom: 1px solid var(--border); } img { max-width: 100%; } }');
    // utilities (tailwind-like)
    const u = [];
    for (let i = 0; i <= 12; i++) { u.push(`.p-${i} { padding: ${i * 4}px; }`, `.m-${i} { margin: ${i * 4}px; }`, `.px-${i} { padding-left: ${i * 4}px; padding-right: ${i * 4}px; }`, `.py-${i} { padding-top: ${i * 4}px; padding-bottom: ${i * 4}px; }`, `.mt-${i} { margin-top: ${i * 4}px; }`, `.mb-${i} { margin-bottom: ${i * 4}px; }`, `.gap-${i} { gap: ${i * 4}px; }`, `.w-${i} { width: ${i * 8}px; }`, `.h-${i} { height: ${i * 8}px; }`); }
    for (const c of ['red', 'green', 'blue', 'gray', 'amber', 'teal']) for (let s = 1; s <= 9; s++) u.push(`.text-${c}-${s} { color: hsl(${rnd(360)}, 60%, ${s * 10}%); }`, `.bg-${c}-${s} { background-color: hsl(${rnd(360)}, 50%, ${s * 10}%); }`, `.border-${c}-${s} { border-color: hsl(${rnd(360)}, 50%, ${s * 10}%); }`);
    u.push('.flex { display: flex; } .grid { display: grid; } .block { display: block; } .hidden { display: none; } .items-center { align-items: center; } .justify-between { justify-content: space-between; } .flex-col { flex-direction: column; } .flex-wrap { flex-wrap: wrap; } .flex-1 { flex: 1 1 0%; } .rounded { border-radius: var(--radius); } .shadow { box-shadow: var(--shadow); } .border { border: 1px solid var(--border); } .text-sm { font-size: 12px; } .text-lg { font-size: 18px; } .font-bold { font-weight: 700; } .truncate { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; } .muted { color: var(--muted); } .relative { position: relative; } .cols-3 { grid-template-columns: repeat(3, 1fr); }');
    for (const bp of [640, 768, 1024, 1280]) for (let i = 0; i <= 12; i += 2) u.push(`@media (min-width: ${bp}px) { .md\\:p-${i}-${bp} { padding: ${i * 4}px; } .md\\:w-${i}-${bp} { width: ${i * 8}px; } }`);
    css.push('@layer utilities { ' + u.join('\n') + ' }');
    // components (BEM, descendants, states)
    const comp = [];
    for (let k = 0; k < 40; k++) {
      const b = '.c' + k;
      comp.push(`${b} { display: block; padding: var(--space-3); border: 1px solid var(--border); border-radius: var(--radius); background: var(--bg); }`,
        `${b}__title { font-size: ${14 + (k % 6)}px; font-weight: 600; margin-bottom: var(--space-2); }`,
        `${b}__body > p:first-child { margin-top: 0; } ${b}__body > p:last-child { margin-bottom: 0; }`,
        `${b}__list > li:nth-child(odd) { background: color-mix(in srgb, var(--accent) 6%, transparent); } ${b}__list > li:nth-child(3n+1) { font-weight: 500; }`,
        `${b}__link:hover, ${b}__link:focus-visible { color: color-mix(in srgb, var(--accent), black 20%); }`,
        `${b}:not(.is-open) ${b}__panel { display: none; } ${b}.is-open ${b}__panel { display: block; }`,
        `${b}[data-state="active"] ${b}__title { color: var(--accent); } ${b}[data-state="active"] { box-shadow: var(--shadow); }`,
        `${b}__meta :is(span, a, time) { color: var(--muted); font-size: 12px; } ${b}__meta .sep::before { content: "\\00b7"; margin: 0 4px; }`,
        `${b}:has(> ${b}__flag) { border-color: var(--accent); }`,
        `@media (max-width: ${600 + k * 5}px) { ${b} { padding: var(--space-2); } ${b}__title { font-size: 14px; } }`,
        `@supports (display: grid) { ${b}__grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(${100 + k}px, 1fr)); gap: var(--space-2); } }`);
    }
    css.push('@layer components { ' + comp.join('\n') + ' }');
    // article (wikipedia-like), long descendant chains
    const art = [];
    for (let i = 0; i < 150; i++) art.push(`.article .sec${i % 12} .para${i % 9} a.l${i % 20} { color: hsl(${i * 7 % 360}, 60%, 35%); }`, `.article table.t${i % 8} tr:nth-child(2n) td:first-child { background: hsl(${i * 11 % 360}, 20%, 95%); }`, `.article .sec${i % 12} > .h${i % 4}::after { content: ""; display: block; border-bottom: 1px solid var(--border); }`);
    css.push(art.join('\n'));
    css.push('.article { max-width: 900px; margin: 0 auto; line-height: 1.6; } .article sup a { font-size: 10px; } .article cite { font-style: normal; color: var(--muted); } .article .infobox { float: right; width: 260px; margin: 0 0 8px 16px; border: 1px solid var(--border); } .app { display: grid; grid-template-columns: 200px 1fr; gap: var(--space-4); } .feed { display: flex; flex-direction: column; gap: var(--space-3); }');
    return css;
  };

  // ---- The document ----
  const page = (feed) => {
    const h = [];
    h.push('<div class="article">');
    for (let s = 0; s < 12; s++) {
      h.push(`<div class="sec${s}"><h2 class="h${s % 4}">Section ${s}</h2>`);
      for (let p = 0; p < 36; p++) {
        let inner = '';
        for (let w = 0; w < 6; w++) inner += `text ${p} <a class="l${rnd(20)}" href="#r${w}">link ${w}</a> and <b>bold</b> <sup><a href="#n${w}">[${w}]</a></sup> `;
        h.push(`<div class="para${p % 9}"><div><p>${inner}<cite>Source ${p}</cite></p></div></div>`);
      }
      if (s % 3 === 0) h.push(`<table class="t${s % 8}">` + Array.from({ length: 12 }, (_, r) => '<tr>' + Array.from({ length: 6 }, (_, c) => `<td><span>${r}.${c}</span></td>`).join('') + '</tr>').join('') + '</table>');
      h.push('</div>');
    }
    h.push('</div><div class="app"><nav class="c1__list"><ul>' + Array.from({ length: 40 }, (_, i) => `<li class="c1__item"><a class="c1__link text-gray-${1 + i % 9} px-2 py-1" href="#">Item ${i}</a></li>`).join('') + '</ul></nav><main>');
    const util = ['flex items-center justify-between p-2 border rounded', 'flex flex-col gap-2 p-4 shadow rounded bg-gray-1', 'grid cols-3 gap-4 py-2 px-4', 'flex flex-wrap gap-1 text-sm muted'];
    for (let i = 0; i < 140; i++) h.push(`<div class="${pick(util)} md:p-${2 * rnd(6)}-768" data-i="${i}"><button class="px-2 py-1 rounded border text-${pick(['red', 'green', 'blue'])}-${1 + rnd(9)}">Act ${i}</button><input class="px-2 py-1 border rounded w-${1 + rnd(12)}" value="v${i}"><span class="truncate text-sm muted">Label ${i}</span><span class="font-bold">${i}</span></div>`);
    h.push('</main></div><div class="feed">');
    for (let i = 0; i < feed; i++) {
      const k = i % 40;
      h.push(`<article class="c${k}${i % 5 === 0 ? ' is-open' : ''}" data-state="${i % 7 === 0 ? 'active' : 'idle'}"><h3 class="c${k}__title">Post ${i}</h3><div class="c${k}__meta"><span>user${i}</span><span class="sep"></span><time>2026-11-${1 + i % 28}</time><a href="#">reply</a></div><div class="c${k}__body"><p>Some text of post ${i} with a <a class="c${k}__link" href="#">link</a> in it.</p><p>Second paragraph of ${i}.</p></div><ul class="c${k}__list">` + Array.from({ length: 5 }, (_, j) => `<li><a href="#" class="c${k}__link">tag ${j}</a></li>`).join('') + `</ul><div class="c${k}__panel p-2"><span>${i}</span></div>${i % 11 === 0 ? `<i class="c${k}__flag"></i>` : ''}</article>`);
    }
    h.push('</div>');
    return h.join('');
  };

  const run = (feed) => {
    const css = sheet();
    const rules = css.join('\n');
    const html = page(feed);
    const t0 = now();
    const style = document.createElement('style'); style.textContent = rules; document.head.appendChild(style);
    const root = document.createElement('div'); root.id = 'root'; root.innerHTML = html; document.body.appendChild(root);
    document.body.offsetHeight;
    const first = now() - t0;
    const all = root.querySelectorAll('*');
    console.log('INFO ' + all.length + ' elements, ' + document.styleSheets[0].cssRules.length + ' top-level rules, feed ' + feed);
    console.log('BENCH style: build page, style and first layout ' + first.toFixed(2) + ' 1');
    const posts = root.querySelectorAll('.feed > article'), midPost = posts[Math.floor(posts.length / 2)];
    const paras = root.querySelectorAll('.article p'), midPara = paras[Math.floor(paras.length / 2)];
    const de = document.documentElement;
    const apps = root.querySelectorAll('.app main > div'), midApp = apps[70];

    // Reads on a clean tree and sweeps of getComputedStyle.
    time('style: getComputedStyle(el).color on every element, clean', () => { for (const e of all) getComputedStyle(e).color; }, { ops: all.length });
    time('style: 20 properties of one element x500, clean', () => { const cs = getComputedStyle(midPost); for (let i = 0; i < 500; i++) { cs.color; cs.fontSize; cs.margin; cs.padding; cs.display; cs.width; cs.height; cs.backgroundColor; cs.borderTopColor; cs.lineHeight; cs.fontWeight; cs.position; cs.flexDirection; cs.gap; cs.boxShadow; cs.opacity; cs.overflow; cs.textAlign; cs.zIndex; cs.visibility; } }, { ops: 10000 });
    // Whole-page restyles.
    time('style: theme switch (data-theme on :root: every variable), then read', () => { de.dataset.theme = de.dataset.theme === 'dark' ? 'light' : 'dark'; midPost.clientWidth; }, { runs: 5 });
    time('style: --accent changed on :root (a variable used in many rules), then read', () => { de.style.setProperty('--accent', '#' + (100000 + rnd(800000))); midPost.clientWidth; }, { runs: 5 });
    time('style: --space changed on :root (calc chains), then read', () => { de.style.setProperty('--space', (3 + rnd(3)) + 'px'); midPost.clientWidth; }, { runs: 5 });
    time('style: class toggle on body, then read', () => { document.body.classList.toggle('x-mode'); midPost.clientWidth; }, { runs: 5 });
    time('style: font-size changed on :root (every em/inherit), then read', () => { de.style.fontSize = (13 + rnd(3)) + 'px'; midPost.clientWidth; }, { runs: 5 });
    // Local changes in a big document.
    time('style: class toggle on one post (is-open: descendant rules), then read', () => { midPost.classList.toggle('is-open'); midPost.clientWidth; }, { runs: 7 });
    time('style: data-state changed on one post (attribute selectors), then read', () => { midPost.dataset.state = midPost.dataset.state === 'active' ? 'idle' : 'active'; midPost.clientWidth; }, { runs: 7 });
    time('style: class toggle on a leaf link, then read', () => { midPost.querySelector('a').classList.toggle('hot'); midPost.clientWidth; }, { runs: 7 });
    time('style: :has() parent, a child flag added and removed, then read', () => { const f = document.createElement('i'); f.className = midPost.className.split(' ')[0] + '__flag'; midPost.appendChild(f); midPost.clientWidth; f.remove(); midPost.clientWidth; }, { runs: 7, ops: 2 });
    time('style: insert an item at the start of a 40-item list (nth-child), then read', () => { const ul = root.querySelector('.c1__list ul'); const li = document.createElement('li'); li.className = 'c1__item'; li.textContent = 'new'; ul.insertBefore(li, ul.firstChild); ul.clientWidth; li.remove(); ul.clientWidth; }, { runs: 7, ops: 2 });
    time('style: inline style on 500 elements, then read', () => { for (let i = 0; i < 500; i++) all[i * 7 % all.length].style.outline = (i % 2) + 'px solid red'; midPost.clientWidth; for (let i = 0; i < 500; i++) all[i * 7 % all.length].style.outline = ''; midPost.clientWidth; }, { runs: 3, ops: 1000 });
    time('style: focus an input (:focus, :focus-within), then read', () => { const i = midApp.querySelector('input'); i.focus(); midApp.clientWidth; i.blur(); midApp.clientWidth; }, { runs: 5, ops: 2 });
    time('style: text change deep in the article, then read', () => { midPara.firstChild.data += '.'; midPara.clientWidth; }, { runs: 7 });
    // Adding things to the page.
    time('style: append 50 posts (innerHTML), then read', () => { const d = document.createElement('div'); d.innerHTML = page(0).slice(0, 0) + Array.from({ length: 50 }, (_, i) => `<article class="c${i % 40}" data-state="idle"><h3 class="c${i % 40}__title">N ${i}</h3><div class="c${i % 40}__body"><p>text <a class="c${i % 40}__link" href="#">l</a></p></div></article>`).join(''); root.querySelector('.feed').appendChild(d); d.clientWidth; d.remove(); root.clientWidth; }, { runs: 5, ops: 2 });
    const lateText = Array.from({ length: 300 }, (_, i) => `.late${i % 30} .c${i % 40}__link { color: rgb(${i % 255}, 80, 90); }`).join('\n');
    let late = null;
    time('style: a late style sheet of 300 rules added (a <style>), then read', () => { late = document.createElement('style'); late.textContent = lateText; document.head.appendChild(late); midPost.clientWidth; }, { runs: 5, setup: () => { if (late) { late.remove(); late = null; midPost.clientWidth; } } });
    time('style: a style sheet of 300 rules removed, then read', () => { late.remove(); late = null; midPost.clientWidth; }, { runs: 5, setup: () => { late = document.createElement('style'); late.textContent = lateText; document.head.appendChild(late); midPost.clientWidth; } });
    time('style: querySelectorAll(".article .sec3 .para4 a.l7")', () => { for (let i = 0; i < 20; i++) root.querySelectorAll('.article .sec3 .para4 a.l7'); }, { ops: 20 });
    time('style: matches() with :not/:is/:has x1000', () => { for (let i = 0; i < 1000; i++) midPost.matches('article:not(.x):has(> .c1__flag), :is(article, div) > h3'); }, { ops: 1000 });
  };

  window.addEventListener('load', () => setTimeout(() => {
    const feeds = (location.search.match(/feed=([\d,]+)/) || [0, '300'])[1].split(',').map(Number);
    for (const f of feeds) { for (const n of [...document.querySelectorAll('#root, style')]) n.remove(); run(f); }
    console.log('BENCHDONE');
  }, 100));
})();
