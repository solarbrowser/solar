(() => {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const send = (text) => { if (typeof __solarUiSend === "function") __solarUiSend(text); };
  const field = $("field"), address = $("address"), shown = $("shown"), site = $("site");
  const lock = '{{icon:security}}';
  const open = '{{icon:security-off}}';
  const globe = site.innerHTML;
  let state = { url: "", title: "", back: false, forward: false, loading: false, tabs: [], active: 0, controls: false, maximized: false };
  const esc = (t) => t.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));

  // The tabs: an icon with the first letter of the site, the title (or the site), a button to close it.
  const tabsBox = $("tabs");
  const nameOf = (t) => {
    if (/^solar:\/\/newtab/.test(t.url) || (!t.url && !t.title)) return "New tab";
    if (t.title && !t.title.startsWith("Solar - ")) return t.title;
    const m = /^[a-z]+:\/\/([^\/?#]*)/.exec(t.url);
    return m ? m[1].replace(/^www\./, "") : t.url;
  };
  const cross = '{{icon:close}}';
  // The pictures the browser fetched for the tabs: kept for the site the tab is on, dropped when the tab goes to another.
  const icons = {}, iconSite = {};
  const siteOf = (url) => { const m = /^[a-z]+:\/\/([^\/?#]*)/.exec(url || ""); return m ? m[1] : ""; };
  // A tab that has no picture of its own (or not yet) has the globe.
  const tabGlobe = '{{icon:public}}';
  let lastTabs = "";
  const renderTabs = () => {
    for (const t of state.tabs) { if (icons[t.id] && iconSite[t.id] !== siteOf(t.url)) { delete icons[t.id]; delete iconSite[t.id]; } }
    for (const id of Object.keys(icons)) { if (!state.tabs.some((t) => String(t.id) === id)) { delete icons[id]; delete iconSite[id]; } }
    const key = JSON.stringify([state.tabs, state.active, Object.keys(icons).map((id) => id + ":" + icons[id].length)]);
    if (key === lastTabs) return;
    lastTabs = key;
    let html = "";
    for (const t of state.tabs) {
      const blank = /^solar:\/\/newtab/.test(t.url) || !t.url;
      const name = nameOf(t);
      html += '<div class="tab' + (t.id === state.active ? " active" : "") + (t.loading ? " loading" : "") + (blank ? " blank" : "") + (t.pinned ? " pinned" : "") + '" data-id="' + t.id + '" title="' + esc(name).replace(/"/g, "&quot;") + '">' + (icons[t.id] && !blank ? '<img class="icon" src="' + icons[t.id] + '">' : '<span class="icon">' + tabGlobe + "</span>") + '<span class="label">' + esc(name) + '</span><button class="close" data-id="' + t.id + '">' + cross + "</button></div>";
    }
    tabsBox.innerHTML = html;
  };
  let lastDrag = "";
  const reportDrag = () => {
    const r = $("drag").getBoundingClientRect();
    const text = Math.round(r.left) + "," + Math.round(r.right);
    if (text !== lastDrag) { lastDrag = text; send("drag\t" + text + "," + Math.round(r.bottom)); }
  };

  // The address as it is shown when it is not being edited: the host in the text colour, the rest dimmed, the scheme left out for the web.
  const render = () => {
    const url = state.url;
    let kind = "";
    let html = "";
    if (!url || /^solar:\/\/newtab/.test(url)) {
      html = "Search or enter an address";
    } else {
      const m = /^(https?):\/\/([^\/?#]*)([^]*)$/.exec(url);
      if (m) {
        kind = m[1] === "https" ? "secure" : "insecure";
        const host = m[2].replace(/^www\./, "");
        const rest = m[3] === "/" ? "" : m[3];
        html = (m[1] === "http" ? "<i>http://</i>" : "") + "<b>" + esc(host) + "</b><i>" + esc(rest) + "</i>";
      } else {
        html = "<b>" + esc(url) + "</b>";
      }
    }
    shown.innerHTML = html;
    field.className = (field.classList.contains("focused") ? "focused " : "") + kind;
    site.innerHTML = kind === "secure" ? lock : kind === "insecure" ? open : globe;
    if (!field.classList.contains("focused")) address.value = /^solar:\/\/newtab/.test(url) ? "" : url;
    $("wmax").innerHTML = state.maximized ? restoreIcon : maxIcon;
    $("wmax").title = state.maximized ? "Restore" : "Maximize";
    $("zoomchip").textContent = (state.zoom || 100) + "%";
    $("menuzoom").textContent = (state.zoom || 100) + "%";
    $("zoomchip").className = state.zoom && state.zoom !== 100 ? "on" : "";
    renderTabs();
    document.documentElement.className = state.controls ? "controlled" : "";
    document.body.className = (state.loading ? "loading " : "") + (state.controls ? "controlled" : "");
    reportDrag();
    $("back").disabled = !state.back;
    $("forward").disabled = !state.forward;
    $("progress").className = state.loading ? "on" : "";
  };

  const shownUrl = () => (/^solar:\/\/newtab/.test(state.url) ? "" : state.url);
  globalThis.__solarUiState = (next) => {
    if (next.openFind) { globalThis.__solarFindUi.open(); return; }
    if (next.find) { globalThis.__solarFindUi.show(next.find); return; }
    if (next.icon) {
      const t = state.tabs.find((x) => x.id === next.icon.tab);
      if (t) { icons[t.id] = next.icon.data; iconSite[t.id] = siteOf(t.url); renderTabs(); }
      return;
    }
    if (next.downloads && globalThis.__solarDownloads) globalThis.__solarDownloads(next.downloads);
    const before = state.active;
    state = Object.assign(state, next);
    render();
    // (Another tab: what was being typed is for the tab it was typed in.)
    if (next.active !== undefined && next.active !== before) { address.value = shownUrl(); if (globalThis.__solarFindUi) globalThis.__solarFindUi.close(); }
    if (next.focusAddress) edit();
    if (next.blur) address.blur();
  };

  const edit = () => { if (!field.classList.contains("focused")) { field.classList.add("focused"); send("focus\t1"); address.value = shownUrl(); } address.focus(); address.select(); };
  const leave = () => { if (field.classList.contains("focused")) { field.classList.remove("focused"); send("focus\t0"); render(); } };

  field.addEventListener("mousedown", () => { if (!field.classList.contains("focused")) setTimeout(edit, 0); });
  address.addEventListener("focus", () => { if (!field.classList.contains("focused")) { field.classList.add("focused"); send("focus\t1"); } });
  address.addEventListener("blur", leave);
  address.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { const text = address.value; leave(); address.blur(); send("go\t" + text); }
    else if (e.key === "Escape") { address.value = shownUrl(); leave(); address.blur(); }
  });
  // (A picture that cannot be shown goes: the letter is there instead.)
  tabsBox.addEventListener("error", (e) => {
    const tab = e.target && e.target.closest ? e.target.closest(".tab") : null;
    if (tab) { delete icons[tab.dataset.id]; lastTabs = ""; renderTabs(); }
  }, true);
  // A tab is dragged along the row (or the column): it follows the pointer, the ones it passes move aside, and where it is let go is where it stays.
  const vertical = document.body.dataset.axis === "y";
  const GAP = 2;
  let drag = null;
  const pointer = (e) => (vertical ? e.clientY : e.clientX);
  const startOf = (r) => (vertical ? r.top : r.left);
  const sizeOf = (r) => (vertical ? r.height : r.width);
  tabsBox.addEventListener("mousedown", (e) => {
    const close = e.target.closest ? e.target.closest(".close") : null;
    const tab = e.target.closest ? e.target.closest(".tab") : null;
    if (close) { send("close\t" + close.dataset.id); return; }
    if (!tab) return;
    if (e.button === 2) { const at = { clientX: e.clientX, clientY: e.clientY }; setTimeout(() => showTabMenu(tab, at), 0); return; }
    send("activate\t" + tab.dataset.id);
    if (e.button !== 0 || tab.classList.contains("pinned")) return;
    const els = Array.from(tabsBox.children);
    drag = { id: Number(tab.dataset.id), els, rects: els.map((el) => el.getBoundingClientRect()), from: els.indexOf(tab), to: els.indexOf(tab), at: pointer(e), moved: false };
  });
  document.addEventListener("mousemove", (e) => {
    if (!drag) return;
    const delta = pointer(e) - drag.at;
    if (!drag.moved && Math.abs(delta) < 6) return;
    drag.moved = true;
    const mine = drag.rects[drag.from], size = sizeOf(mine) + GAP;
    // (It stays inside the row.)
    const lowest = startOf(drag.rects[0]) - startOf(mine);
    const last = drag.rects[drag.rects.length - 1];
    const highest = startOf(last) + sizeOf(last) - startOf(mine) - sizeOf(mine);
    const d = Math.max(lowest, Math.min(highest, delta));
    const centre = startOf(mine) + sizeOf(mine) / 2 + d;
    let to = 0;
    drag.rects.forEach((r, j) => { if (j !== drag.from && startOf(r) + sizeOf(r) / 2 < centre) to++; });
    to = Math.max(to, drag.els.filter((el) => el.classList.contains("pinned")).length);
    drag.to = to;
    drag.els.forEach((el, j) => {
      let shift = 0;
      if (j === drag.from) shift = d;
      else if (drag.from < j && j <= to) shift = -size;
      else if (to <= j && j < drag.from) shift = size;
      el.style.transform = (vertical ? "translateY(" : "translateX(") + shift + "px)";
    });
    drag.els[drag.from].style.zIndex = "5";
  });
  document.addEventListener("mouseup", () => {
    if (!drag) return;
    const done = drag;
    drag = null;
    for (const el of done.els) { el.style.transform = ""; el.style.zIndex = ""; }
    if (done.moved && done.to !== done.from) {
      // (The order is changed here at once, and the browser keeps it.)
      const moved = state.tabs.splice(done.from, 1)[0];
      state.tabs.splice(done.to, 0, moved);
      lastTabs = "";
      renderTabs();
      send("move\t" + done.id + "\t" + done.to);
    }
  });
  $("newtab").addEventListener("click", () => send("newtab"));
  $("zoomchip").addEventListener("click", () => send("zoom\t0"));
  // The menu comes down over the page: the browser makes this page taller for as long as it is open.
  const menu = $("menu");
  let menuOpen = false;
  // The tab's menu (the right button on a tab): pin or unpin it, close it, a new tab.
  const tabMenu = document.createElement("div");
  tabMenu.id = "tabmenu";
  document.body.appendChild(tabMenu);
  let tabMenuFor = 0, lastOverlay = "0";
  const barTop = Number(document.body.dataset.top || 88);
  const FIND_WIDTH = 372;
  let findOpen = false, dlOpen = false, dlOverlay = 0;
  const syncOverlay = () => {
    // (A menu has the window; the find box has only the place it is in.)
    let wanted = menuOpen ? 330 : tabMenuFor ? 150 : dlOpen ? dlOverlay : findOpen ? 62 : 0;
    const place = !menuOpen && !tabMenuFor && !dlOpen && findOpen ? "\t" + Math.max(0, window.innerWidth - FIND_WIDTH - 16) + "\t" + (window.innerWidth - 8) : "";
    const key = wanted + place;
    if (key !== lastOverlay) { lastOverlay = key; send("overlay\t" + wanted + place); }
  };
  const hideTabMenu = () => { if (tabMenuFor) { tabMenuFor = 0; tabMenu.className = ""; syncOverlay(); } };
  const showTabMenu = (tab, e) => {
    const id = Number(tab.dataset.id), pinned = tab.classList.contains("pinned");
    tabMenuFor = id;
    tabMenu.innerHTML = '<button class="item" data-do="pin\t' + id + "\t" + (pinned ? "0" : "1") + '"><span>' + (pinned ? "Unpin tab" : "Pin tab") + '</span></button><button class="item" data-do="close\t' + id + '"><span>Close tab</span></button><hr><button class="item" data-do="newtab"><span>New tab</span></button>';
    tabMenu.setAttribute("style", "left:" + Math.max(4, Math.min(e.clientX, window.innerWidth - 196)) + "px;top:" + (e.clientY + 6) + "px");
    tabMenu.className = "open";
    syncOverlay();
  };
  tabMenu.addEventListener("click", (e) => {
    const item = e.target.closest ? e.target.closest(".item") : null;
    if (!item) return;
    hideTabMenu();
    send(item.dataset.do);
  });
  // The find box (Ctrl+F): over the page, at the top right; what is typed is looked for in the page, Enter goes to the next match, Shift+Enter to the one before, Escape closes it.
  const findBar = document.createElement("div");
  findBar.id = "findbar";
  findBar.innerHTML = '<input type="text" spellcheck="false" autocomplete="off" placeholder="Find in page"><span class="count"></span><button class="prev" title="Previous">{{icon:chevron-left}}</button><button class="next" title="Next">{{icon:chevron-right}}</button><button class="shut" title="Close">{{icon:close}}</button>';
  document.body.appendChild(findBar);
  const findInput = findBar.querySelector("input"), findCount = findBar.querySelector(".count");
  const findAsk = (what) => send("find\t" + findInput.value + "\t" + what);
  const showFind = (result) => {
    if (!findInput.value) { findCount.textContent = ""; findCount.className = "count"; return; }
    findCount.textContent = result && result.count ? result.index + "/" + result.count : "0/0";
    findCount.className = "count" + (result && result.count ? "" : " none");
  };
  const closeFind = () => {
    if (!findOpen) return;
    findOpen = false;
    findBar.className = "";
    send("find\t\t3");
    syncOverlay();
  };
  const openFind = () => {
    findBar.setAttribute("style", "top:" + (barTop + 8) + "px;left:" + Math.max(0, window.innerWidth - FIND_WIDTH - 16) + "px");
    if (!findOpen) { findOpen = true; findBar.className = "open"; syncOverlay(); }
    findInput.focus();
    findInput.select();
    if (findInput.value) findAsk(0);
  };
  findInput.addEventListener("input", () => { if (findInput.value) findAsk(0); else { send("find\t\t3"); showFind(null); } });
  findInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") findAsk(e.shiftKey ? 2 : 1);
    else if (e.key === "Escape") closeFind();
  });
  findBar.querySelector(".prev").addEventListener("click", () => findAsk(2));
  findBar.querySelector(".next").addEventListener("click", () => findAsk(1));
  findBar.querySelector(".shut").addEventListener("click", closeFind);
  globalThis.__solarFindUi = { open: () => setTimeout(openFind, 0), show: showFind, close: closeFind, isOpen: () => findOpen };
  // The downloads: a button that is there while there is something in the list, and the list (a file is opened by pressing it, a download that goes on is stopped by its cross).
  const dlPanel = document.createElement("div");
  dlPanel.id = "dlpanel";
  document.body.appendChild(dlPanel);
  const dlButton = $("dlbutton");
  let downloads = [], seenDownload = 0;
  const bytes = (n) => (n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(0) + " KB" : n < 1073741824 ? (n / 1048576).toFixed(1) + " MB" : (n / 1073741824).toFixed(2) + " GB");
  const renderDownloads = () => {
    let html = '<div class="head"><span>Downloads</span>' + (downloads.some((d) => d.state !== "running") ? '<button data-do="dl-clear">Clear</button>' : "") + "</div>";
    if (!downloads.length) html += '<div class="none">Nothing has been downloaded.</div>';
    for (const d of downloads) {
      let meta = "", bar = "", action = "";
      if (d.asking) {
        meta = "Choose where to keep it";
        action = '<button data-do="dl-cancel\t' + d.id + '" title="Cancel">{{icon:close}}</button>';
      } else if (d.state === "running") {
        meta = d.total ? bytes(d.received) + " of " + bytes(d.total) : bytes(d.received);
        bar = '<div class="bar"><i style="width:' + (d.total ? Math.min(100, Math.round(100 * d.received / d.total)) : 30) + '%"></i></div>';
        action = '<button data-do="dl-cancel\t' + d.id + '" title="Cancel">{{icon:close}}</button>';
      } else if (d.state === "done") {
        meta = bytes(d.total || d.received);
        action = '<button data-do="dl-folder\t' + d.id + '" title="Show in folder">{{icon:open-in-new}}</button>';
      } else {
        meta = d.state === "failed" ? "Failed" : "Cancelled";
      }
      html += '<div class="file" data-open="' + (d.state === "done" ? d.id : 0) + '"><div class="info"><div class="name">' + esc(d.name) + '</div><div class="meta' + (d.state === "failed" ? " failed" : "") + '">' + meta + "</div>" + bar + "</div>" + action + "</div>";
    }
    html += '<button class="all" data-do="downloads-page">Show all downloads</button>';
    dlPanel.innerHTML = html;
    // (How tall the interface is to be while the list is open: the list, and room around it.)
    dlOverlay = 4 + 44 + Math.max(1, downloads.length) * 56 + 50 + (88 - barTop);
  };
  const placeDownloads = () => {
    // (Under the button, at the right; in the column that has everything, above its foot.)
    if (barTop === 0) dlPanel.setAttribute("style", "left:10px;bottom:56px");
    else dlPanel.setAttribute("style", "right:10px;top:" + (barTop + 4) + "px");
  };
  const setDownloads = (open) => {
    if (open === dlOpen) return;
    dlOpen = open;
    dlPanel.className = open ? "open" : "";
    if (open) { renderDownloads(); placeDownloads(); }
    syncOverlay();
  };
  // The button goes to the page of the downloads; the list that comes down is what shows when a download begins.
  dlButton.addEventListener("click", () => { setDownloads(false); send("downloads-page"); });
  dlPanel.addEventListener("mousedown", (e) => {
    const act = e.target.closest ? e.target.closest("button") : null;
    const file = e.target.closest ? e.target.closest(".file") : null;
    if (act && act.dataset.do) { send(act.dataset.do); if (act.dataset.do === "downloads-page") setDownloads(false); return; }
    if (file && Number(file.dataset.open) > 0) send("dl-open\t" + file.dataset.open);
  });
  dlPanel.addEventListener("click", (e) => {
    const act = e.target.closest ? e.target.closest("button") : null;
    if (act && act.dataset.do === "dl-clear") send("dl-clear");
  });
  globalThis.__solarDownloads = (list) => {
    downloads = list;
    const running = list.some((d) => d.state === "running");
    dlButton.className = list.length ? "on" + (running ? " busy" : "") : "";
    // A download that is new has its list shown.
    const newest = list.reduce((m, d) => Math.max(m, d.id), 0);
    if (newest > seenDownload) { seenDownload = newest; setTimeout(() => setDownloads(true), 0); }
    if (dlOpen) renderDownloads();
    if (dlOpen && !list.length) setTimeout(() => setDownloads(false), 0);
  };
  const setMenu = (open) => {
    if (open === menuOpen) return;
    menuOpen = open;
    menu.className = open ? "open" : "";
    syncOverlay();
  };
  $("menuButton").addEventListener("click", () => setMenu(!menuOpen));
  menu.addEventListener("click", (e) => {
    // (The zoom is changed with the menu left open, so that it can be changed again.)
    const zoom = e.target.closest ? e.target.closest("[data-zoom]") : null;
    if (zoom) { send("zoom\t" + zoom.dataset.zoom); return; }
    const item = e.target.closest ? e.target.closest(".item") : null;
    if (!item || !item.dataset.do) return;
    setMenu(false);
    if (item.dataset.do === "downloads") send("downloads-page");
    else if (item.dataset.do === "find") globalThis.__solarFindUi.open();
    else send(item.dataset.do);
  });
  document.addEventListener("mousedown", (e) => {
    if (menuOpen && !(e.target.closest && (e.target.closest("#menu") || e.target.closest("#menuButton")))) setMenu(false);
    if (tabMenuFor && !(e.target.closest && e.target.closest("#tabmenu"))) hideTabMenu();
    if (dlOpen && !(e.target.closest && (e.target.closest("#dlpanel") || e.target.closest("#dlbutton")))) setDownloads(false);
  });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") { if (menuOpen) setMenu(false); hideTabMenu(); setDownloads(false); } });
  $("wmin").addEventListener("click", () => send("min"));
  $("wmax").addEventListener("click", () => send("max"));
  // The buttons of the window are drawn alike (one line, one weight), not taken from the set of icons, whose signs for them are of different weights.
  const stroke = (shapes) => '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">' + shapes + "</svg>";
  const minIcon = stroke('<path d="M3.5 8h9"/>');
  const maxIcon = stroke('<rect x="3.5" y="3.5" width="9" height="9" rx="1.8"/>');
  const restoreIcon = stroke('<rect x="3.5" y="6" width="6.5" height="6.5" rx="1.5"/><path d="M6 6V5a1.5 1.5 0 0 1 1.5-1.5H11A1.5 1.5 0 0 1 12.5 5v3.5A1.5 1.5 0 0 1 11 10h-1"/>');
  $("wmin").innerHTML = minIcon;
  $("wmax").innerHTML = maxIcon;
  $("wclose").innerHTML = stroke('<path d="M4.2 4.2l7.6 7.6M11.8 4.2l-7.6 7.6"/>');
  $("wclose").addEventListener("click", () => send("quit"));
  window.addEventListener("resize", reportDrag);
  $("back").addEventListener("click", () => send("back"));
  $("forward").addEventListener("click", () => send("forward"));
  $("reload").addEventListener("click", () => send("reload"));
  render();
  send("ready");
})();
