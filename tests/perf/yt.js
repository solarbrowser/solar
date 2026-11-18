// A page of the kind a video site is: a masthead, a player in a container (with an overlay of controls and an ad whose parts move by transitions and animations of bottom and opacity), the title
// and the channel, a description, a hundred comments, a column of related videos; custom-element-like tags, flex everywhere, variables, @container rules. Each prints one line
// "BENCH <name> <ms per operation> <operations>". Run: tests/perf/run.py --suite yt
(function () {
  const now = () => performance.now();
  const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
  const only = (location.search.match(/only=([^&]+)/) || [0, ''])[1];
  const onlyRe = only ? new RegExp(decodeURIComponent(only)) : null;
  const wanted = (name) => !onlyRe || onlyRe.test(name);
  const time = (name, op, options = {}) => {
    if (!wanted(name)) return;
    const runs = options.runs || 5, samples = [];
    for (let r = 0; r < runs; r++) { if (options.setup) options.setup(); document.body.offsetHeight; const t0 = now(); op(); samples.push(now() - t0); }
    console.log('BENCH ' + name + ' ' + (median(samples) / (options.ops || 1)).toFixed(4) + ' ' + (options.ops || 1));
  };
  // A frame at a time: `step(i)` changes something and reads a size; the median of the cost of a step.
  const frames = (name, count, step) => new Promise((resolve) => {
    if (!wanted(name)) return resolve();
    const samples = []; let i = 0;
    const tick = () => { const a = now(); step(i); document.body.offsetHeight; samples.push(now() - a); if (++i < count) requestAnimationFrame(tick); else { console.log('BENCH ' + name + ' ' + median(samples).toFixed(4) + ' 1'); resolve(); } };
    requestAnimationFrame(tick);
  });
  let seed = 777;
  const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };

  const css = `
:root { --yt-bg: #0f0f0f; --yt-fg: #f1f1f1; --yt-muted: #aaa; --yt-accent: #3ea6ff; --yt-border: rgba(255,255,255,.1); --yt-gap: 12px; --yt-radius: 12px; --font: Roboto, Arial, sans-serif; }
html { font: 14px/1.4 var(--font); color: var(--yt-fg); background: var(--yt-bg); }
body { margin: 0; }
ytd-app, ytd-masthead, ytd-watch-flexy, ytd-comments, ytd-comment-thread-renderer, ytd-comment-renderer, ytd-compact-video-renderer, ytd-watch-metadata, ytd-video-owner-renderer { display: block; }
#masthead { display: flex; align-items: center; height: 56px; padding: 0 16px; gap: 16px; border-bottom: 1px solid var(--yt-border); }
#masthead .search { flex: 1; display: flex; max-width: 640px; } #masthead input { flex: 1; height: 32px; border: 1px solid var(--yt-border); border-radius: 16px 0 0 16px; background: transparent; color: inherit; padding: 0 12px; }
#columns { display: flex; gap: 24px; padding: 24px; }
#primary { flex: 1 1 0; min-width: 0; } #secondary { width: 402px; flex: none; }
.player { container-type: inline-size; container-name: player; position: relative; aspect-ratio: 16/9; background: #000; border-radius: var(--yt-radius); overflow: hidden; }
.player .video { position: absolute; inset: 0; }
.ytp-chrome-bottom { position: absolute; left: 12px; right: 12px; bottom: 12px; height: 48px; display: flex; align-items: center; gap: 8px; transition: opacity .25s; }
.ytp-chrome-bottom .ytp-button { width: 36px; height: 36px; border-radius: 50%; background: rgba(255,255,255,.1); }
.player.ytp-chrome-hidden .ytp-chrome-bottom { opacity: 0; }
.ytp-ad-player-overlay-layout__player-card { position: absolute; left: 0; bottom: 60px; width: 40%; display: flex; gap: 8px; padding: 8px; background: rgba(0,0,0,.7); transition: bottom .3s ease; }
.ytp-ad-player-overlay-layout__ad-info { position: absolute; right: 12px; bottom: 60px; transition: bottom .3s ease; }
.ytp-ad-persistent-progress-bar-container { position: absolute; left: 0; right: 0; bottom: 0; height: 3px; transition: opacity .2s, bottom .2s; }
.ytp-ad-persistent-progress-bar { height: 100%; background: #fc0; width: 30%; }
@container player (min-width: 600px) { .ytp-chrome-bottom .ytp-button { width: 40px; height: 40px; } .ytp-ad-player-overlay-layout__player-card { padding: 12px; gap: 12px; } }
@container player (max-width: 599px) { .ytp-chrome-bottom .ytp-time { display: none; } }
#title h1 { font-size: 20px; font-weight: 600; margin: 12px 0 8px; }
#owner { display: flex; align-items: center; gap: var(--yt-gap); }
#owner .avatar { width: 40px; height: 40px; border-radius: 50%; background: #333; }
#owner .buttons { margin-left: auto; display: flex; gap: 8px; } .chip { padding: 0 16px; height: 36px; line-height: 36px; border-radius: 18px; background: rgba(255,255,255,.1); }
#description { margin-top: 12px; padding: 12px; border-radius: var(--yt-radius); background: rgba(255,255,255,.1); }
ytd-comment-thread-renderer { padding: 8px 0; } ytd-comment-renderer { display: flex; gap: 16px; }
ytd-comment-renderer .avatar { width: 40px; height: 40px; border-radius: 50%; background: #333; flex: none; } ytd-comment-renderer .main { flex: 1; min-width: 0; }
ytd-comment-renderer .author { font-weight: 500; font-size: 13px; } ytd-comment-renderer .text { margin: 4px 0; white-space: pre-wrap; }
ytd-comment-renderer .toolbar { display: flex; gap: 8px; color: var(--yt-muted); }
ytd-compact-video-renderer { display: flex; gap: 8px; margin-bottom: 8px; } ytd-compact-video-renderer .thumb { width: 168px; height: 94px; border-radius: 8px; background: #222; flex: none; position: relative; }
ytd-compact-video-renderer .thumb .time { position: absolute; right: 4px; bottom: 4px; padding: 1px 4px; background: rgba(0,0,0,.8); border-radius: 4px; font-size: 12px; }
ytd-compact-video-renderer .meta { min-width: 0; } ytd-compact-video-renderer .title { font-size: 14px; font-weight: 500; max-height: 40px; overflow: hidden; }
ytd-compact-video-renderer .byline { color: var(--yt-muted); font-size: 12px; }
.ripple { position: relative; overflow: hidden; } .ripple::after { content: ""; position: absolute; inset: 0; background: currentColor; opacity: 0; transition: opacity .15s; } .ripple:hover::after { opacity: .1; }
@media (max-width: 1000px) { #secondary { display: none; } }
`;

  const build = () => {
    const style = document.createElement('style'); style.textContent = css; document.head.appendChild(style);
    const h = [];
    h.push('<ytd-app><ytd-masthead><div id="masthead"><div class="chip ripple">menu</div><div class="logo">Video</div><div class="search"><input value="search"><div class="chip ripple">go</div></div><div class="chip ripple">create</div><div class="avatar chip ripple">me</div></div></ytd-masthead>');
    h.push('<ytd-watch-flexy><div id="columns"><div id="primary"><div class="player" id="player"><div class="video"></div><div class="ytp-ad-player-overlay-layout__player-card" id="adcard"><div class="adimg"></div><div class="adtext"><span>Ad</span> <span>Sponsored content here</span></div></div><div class="ytp-ad-player-overlay-layout__ad-info" id="adinfo"><span>Ad 1 of 2</span></div><div class="ytp-ad-persistent-progress-bar-container" id="adbar"><div class="ytp-ad-persistent-progress-bar"></div></div><div class="ytp-chrome-bottom" id="chrome">' + Array.from({ length: 8 }, (_, i) => `<div class="ytp-button ripple" data-i="${i}"></div>`).join('') + '<span class="ytp-time">0:12 / 3:34</span></div></div>');
    h.push('<ytd-watch-metadata><div id="title"><h1>A video title that is long enough to take a line or two in the column of the page</h1></div><ytd-video-owner-renderer><div id="owner"><div class="avatar"></div><div><div>Channel name</div><div class="byline">1.2M subscribers</div></div><div class="buttons"><div class="chip ripple">Join</div><div class="chip ripple">Subscribe</div><div class="chip ripple">Like</div><div class="chip ripple">Share</div></div></div></ytd-video-owner-renderer><div id="description">' + 'Description text of the video with some words in it. '.repeat(20) + '</div></ytd-watch-metadata>');
    h.push('<ytd-comments id="comments">');
    for (let i = 0; i < 100; i++) h.push(`<ytd-comment-thread-renderer><ytd-comment-renderer><div class="avatar"></div><div class="main"><div class="author">@user${i}</div><div class="text">Comment ${i}: ${'some words of a comment '.repeat(1 + rnd(5))}</div><div class="toolbar"><span class="ripple chip">like</span><span>${rnd(900)}</span><span class="ripple chip">reply</span></div></div></ytd-comment-renderer></ytd-comment-thread-renderer>`);
    h.push('</ytd-comments></div><div id="secondary">');
    for (let i = 0; i < 40; i++) h.push(`<ytd-compact-video-renderer><div class="thumb ripple"><span class="time">${rnd(10)}:${10 + rnd(49)}</span></div><div class="meta"><div class="title">Related video number ${i} with a title of some length</div><div class="byline">Channel ${i}</div><div class="byline">${rnd(900)}K views</div></div></ytd-compact-video-renderer>`);
    h.push('</div></div></ytd-watch-flexy></ytd-app>');
    const root = document.createElement('div'); root.id = 'root'; root.innerHTML = h.join(''); document.body.appendChild(root);
    return root;
  };

  const main = async () => {
    const t0 = now();
    const root = build();
    document.body.offsetHeight;
    console.log('INFO ' + document.querySelectorAll('*').length + ' elements');
    console.log('BENCH yt: build page, style and first layout ' + (now() - t0).toFixed(2) + ' 1');
    const player = document.getElementById('player'), adcard = document.getElementById('adcard'), adinfo = document.getElementById('adinfo'), adbar = document.getElementById('adbar'), chrome = document.getElementById('chrome');
    const comments = document.getElementById('comments'), related = document.getElementById('secondary');
    // An animation of `bottom` in a container, a frame at a time (the ad overlay of a video): the same animation running.
    const slide = adcard.animate([{ bottom: '60px' }, { bottom: '90px' }], { duration: 100000, fill: 'both' });
    await frames('yt: one running animation of bottom in the player, one frame', 60, () => {});
    slide.cancel();
    // A new animation at each frame (what the ad does at each tick: its parts are started again).
    await frames('yt: a new animation of bottom started at each frame (in the player)', 60, (i) => { adcard.animate([{ bottom: '60px' }, { bottom: (70 + i % 20) + 'px' }], { duration: 300 }); adinfo.animate([{ bottom: '60px' }, { bottom: '80px' }], { duration: 300 }); });
    await frames('yt: opacity fade restarted at each frame', 60, (i) => { adbar.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 200 }); chrome.animate([{ opacity: 1 }, { opacity: 0.5 }], { duration: 200 }); });
    await frames('yt: width of the progress bar changed at each frame (a size, in the player)', 60, (i) => { adbar.firstElementChild.style.width = (i % 100) + '%'; });
    await frames('yt: width of the player changed at each frame (a container)', 30, (i) => { player.style.width = (640 + (i % 2) * 20) + 'px'; });
    player.style.width = '';
    time('yt: class toggle on the player (controls hidden), then read', () => { player.classList.toggle('ytp-chrome-hidden'); document.body.offsetHeight; }, { runs: 7 });
    time('yt: 20 comments appended, then read', () => { const d = document.createElement('div'); d.innerHTML = Array.from({ length: 20 }, (_, i) => `<ytd-comment-thread-renderer><ytd-comment-renderer><div class="avatar"></div><div class="main"><div class="author">@new${i}</div><div class="text">New comment ${i} with some words</div><div class="toolbar"><span class="ripple chip">like</span></div></div></ytd-comment-renderer></ytd-comment-thread-renderer>`).join(''); comments.appendChild(d); document.body.offsetHeight; d.remove(); }, { runs: 5 });
    time('yt: text of one related video changed, then read', () => { related.querySelector('.title').firstChild.data += '.'; document.body.offsetHeight; }, { runs: 7 });
    time('yt: hover state on a related video thumb (a ripple), then read', () => { const t = related.querySelector('.thumb'); t.classList.toggle('hot'); document.body.offsetHeight; }, { runs: 7 });
    console.log('BENCHDONE');
  };
  window.addEventListener('load', () => setTimeout(main, 100));
})();
