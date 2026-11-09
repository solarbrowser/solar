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
      html += '<div class="tab' + (t.id === state.active ? " active" : "") + (t.loading ? " loading" : "") + (blank ? " blank" : "") + (t.pinned ? " pinned" : "") + '" data-id="' + t.id + '" title="' + esc(name).replace(/"/g, "&quot;") + '">' + (icons[t.id] && !blank ? '<img class="icon" src="' + icons[t.id] + '">' : '<span class="icon"></span>') + '<span class="label">' + esc(name) + '</span><button class="close" data-id="' + t.id + '">' + cross + "</button></div>";
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
    if (next.icon) {
      const t = state.tabs.find((x) => x.id === next.icon.tab);
      if (t) { icons[t.id] = next.icon.data; iconSite[t.id] = siteOf(t.url); renderTabs(); }
      return;
    }
    const before = state.active;
    state = Object.assign(state, next);
    render();
    // (Another tab: what was being typed is for the tab it was typed in.)
    if (next.active !== undefined && next.active !== before) address.value = shownUrl();
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
  // The menu comes down over the page: the browser makes this page taller for as long as it is open.
  const menu = $("menu");
  let menuOpen = false;
  // The tab's menu (the right button on a tab): pin or unpin it, close it, a new tab.
  const tabMenu = document.createElement("div");
  tabMenu.id = "tabmenu";
  document.body.appendChild(tabMenu);
  let tabMenuFor = 0, lastOverlay = 0;
  const syncOverlay = () => {
    const wanted = menuOpen ? 88 + 4 + 3 * 38 + 2 * 13 + 20 : tabMenuFor ? 150 : 0;
    if (wanted !== lastOverlay) { lastOverlay = wanted; send("overlay\t" + wanted); }
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
  const setMenu = (open) => {
    if (open === menuOpen) return;
    menuOpen = open;
    menu.className = open ? "open" : "";
    syncOverlay();
  };
  $("menuButton").addEventListener("click", () => setMenu(!menuOpen));
  menu.addEventListener("click", (e) => {
    const item = e.target.closest ? e.target.closest(".item") : null;
    if (!item) return;
    setMenu(false);
    send(item.dataset.do);
  });
  document.addEventListener("mousedown", (e) => {
    if (menuOpen && !(e.target.closest && (e.target.closest("#menu") || e.target.closest("#menuButton")))) setMenu(false);
    if (tabMenuFor && !(e.target.closest && e.target.closest("#tabmenu"))) hideTabMenu();
  });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") { if (menuOpen) setMenu(false); hideTabMenu(); } });
  $("wmin").addEventListener("click", () => send("min"));
  $("wmax").addEventListener("click", () => send("max"));
  const maxIcon = '{{icon:crop-square}}', restoreIcon = '{{icon:image-filter-none}}';
  $("wclose").addEventListener("click", () => send("quit"));
  window.addEventListener("resize", reportDrag);
  $("back").addEventListener("click", () => send("back"));
  $("forward").addEventListener("click", () => send("forward"));
  $("reload").addEventListener("click", () => send("reload"));
  render();
  send("ready");
})();
