// Helix Settings. Rust owns every picture and every request: this page draws views it was sent (text only through textContent) and sends back
// the names those views listed. Nothing here builds a command or names a client it was not given.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

$("close").onclick = () => window.__TAURI__.window.getCurrentWindow().close();
$("refresh").onclick = () => invoke("agents_refresh");

let reporting = false;
let reportTimer = null;

// Pages -----------------------------------------------------------------------------------------------------------------------------
const PAGES = [
  { name: "appearance", title: "Appearance", icon: "palette", rows: true },
  { name: "wallpaper", title: "Wallpaper", icon: "image", rows: false },
  { name: "accessibility", title: "Accessibility", icon: "eye", rows: true },
  { name: "displays", title: "Displays", icon: "sun", rows: true },
  { name: "network", title: "Network", icon: "wifi", rows: false },
  { name: "bluetooth", title: "Bluetooth", icon: "bluetooth", rows: false },
  { name: "keyboard", title: "Keyboard", icon: "keyboard", rows: false },
  { name: "sound", title: "Sound", icon: "volume", rows: true },
  { name: "dock", title: "Dock & corners", icon: "grid", rows: true },
  { name: "notifications", title: "Notifications", icon: "bell", rows: true },
  { name: "power", title: "Power & lock", icon: "lock", rows: true },
  { name: "agents", title: "Privacy & agents", icon: "shield", rows: false },
  { name: "about", title: "About", icon: "sparkle", rows: false },
];
const TITLES = Object.fromEntries(PAGES.map((page) => [page.name, page.title]));
function buildPages() {
  const nav = $("pages");
  const content = $("content");
  const agents = $("page-agents");
  nav.replaceChildren();
  PAGES.forEach((page) => {
    const link = el("button", "page-link");
    link.dataset.page = page.name;
    link.dataset.test = "page-" + page.name;
    link.append(icon(page.icon), el("span", "", page.title));
    link.onclick = () => showPage(page.name);
    nav.append(link);
    if (page.name === "network" || page.name === "bluetooth" || page.name === "about" || page.name === "keyboard" || page.name === "wallpaper") {
      const section = el("section", "settings-page");
      section.id = "page-" + page.name;
      section.hidden = true;
      const host = el("div");
      host.id = page.name + "-host";
      if (page.name === "bluetooth") {
        const pairing = el("div");
        pairing.id = "bt-pairing";
        const main = el("div");
        main.id = "bt-main";
        host.append(pairing, main);
      }
      section.append(host);
      content.insertBefore(section, agents);
    }
    if (page.rows) {
      const section = el("section", "settings-page");
      section.id = "page-" + page.name;
      section.hidden = true;
      const rows = el("div");
      rows.id = page.name + "-rows";
      section.append(rows);
      content.insertBefore(section, agents);
    }
  });
}
let current = "appearance";
function showPage(name) {
  current = name;
  $("search").value = "";
  document.querySelectorAll(".settings-page").forEach((node) => { node.hidden = node.id !== "page-" + name; });
  document.querySelectorAll(".page-link").forEach((node) => node.classList.toggle("current", node.dataset.page === name));
  $("page-title").textContent = TITLES[name];
  $("refresh").style.display = name === "agents" ? "" : "none";
  if (name === "about") drawAbout();
  if (name === "keyboard") drawKeyboard();
  if (name === "wallpaper") invoke("wallpaper_view").then(drawWallpaper);
  queueReport();
}
buildPages();
showPage("appearance");

// About: what this copy is and what it can reach -----------------------------------------------------------------------------------
async function drawAbout() {
  const info = await invoke("about");
  const host = $("about-host");
  host.replaceChildren();
  const card = el("div", "rows-card");
  [["Helix Settings", info.version], ["Desktop service", info.service], ["Agent access", info.agent_access],
   ["Settings on these pages", String(info.settings)], ["Wi-Fi", info.network ? "Available" : "Not available"], ["Bluetooth", info.bluetooth ? "Available" : "Not available"]]
    .forEach(([name, value]) => {
      const line = el("div", "setting-row");
      line.append(el("div", "setting-title", name), el("div", "slider-value", value));
      card.append(line);
    });
  host.append(card);
  queueReport();
}

// Wallpaper: a grid of the pictures in the folder; choosing one writes the choice and the wallpaper process reloads by itself ---------
let thumbObserver = null;
function drawWallpaper(view) {
  const host = $("wallpaper-host");
  host.replaceChildren();
  host.append(el("h4", "", "Colours"));
  const swatches = el("div", "swatches");
  view.swatches.forEach((swatch) => {
    const button = el("button", "swatch" + (swatch.current ? " current" : ""));
    button.style.background = swatch.hex;
    button.title = swatch.hex;
    button.dataset.test = "wp-swatch-" + swatch.index;
    button.onclick = () => invoke("wallpaper_choose_swatch", { index: swatch.index });
    swatches.append(button);
  });
  host.append(swatches);
  host.append(el("h4", "", "Find on Wallhaven"));
  host.append(wallhavenBlock());
  host.append(el("h4", "", "Pictures"));
  host.append(el("div", "setting-about note", view.folder));
  const grid = el("div", "wp-grid");
  if (thumbObserver) thumbObserver.disconnect();
  thumbObserver = new IntersectionObserver((entries) => entries.forEach((entry) => {
    if (!entry.isIntersecting) return;
    const tile = entry.target;
    thumbObserver.unobserve(tile);
    invoke("wallpaper_thumb", { index: Number(tile.dataset.index) }).then((url) => {
      if (url) tile.style.backgroundImage = "url(" + url + ")";
    });
  }), { rootMargin: "200px" });
  view.images.forEach((image) => {
    const tile = el("button", "wp-tile" + (image.current ? " current" : ""));
    tile.dataset.index = image.index;
    tile.dataset.test = "wp-image-" + image.index;
    tile.title = image.name;
    tile.append(el("span", "wp-name", image.name));
    tile.onclick = () => invoke("wallpaper_choose_image", { index: image.index });
    grid.append(tile);
    thumbObserver.observe(tile);
  });
  host.append(grid);
  if (!view.images.length) host.append(el("div", "empty", "No pictures in this folder."));
  queueReport();
}
listen("wallpaper-view", (event) => drawWallpaper(event.payload));

// Wallhaven: search wallhaven.cc (safe-for-work only). The block is built once, so choosing a picture does not lose the results. -----
let wallhaven = null;
function wallhavenBlock() {
  if (wallhaven) return wallhaven.root;
  const root = el("div", "wh");
  const bar = el("form", "wh-bar");
  const query = el("input");
  query.type = "search"; query.placeholder = "Search wallpapers"; query.autocomplete = "off"; query.spellcheck = false;
  query.dataset.test = "wh-query";
  const sorting = el("select");
  sorting.dataset.test = "wh-sorting";
  [["toplist", "Top this year"], ["latest", "Latest"], ["random", "Random"], ["views", "Most viewed"], ["favorites", "Most favourited"]]
    .forEach(([value, label]) => { const o = el("option", "", label); o.value = value; sorting.append(o); });
  const minimum = el("select");
  minimum.dataset.test = "wh-minimum";
  [["1080", "Full HD and up"], ["1440", "QHD and up"], ["2160", "4K and up"], ["any", "Any size"]]
    .forEach(([value, label]) => { const o = el("option", "", label); o.value = value; minimum.append(o); });
  const go = el("button", "primary", "Search");
  go.type = "submit"; go.dataset.test = "wh-search";
  bar.append(query, sorting, minimum, go);
  const note = el("div", "setting-about note", "Searching sends your words to wallhaven.cc. Only safe-for-work pictures are shown.");
  const message = el("div", "setting-about warn");
  const surprise = el("button", "", "Surprise me");
  surprise.type = "button"; surprise.dataset.test = "wh-surprise";
  bar.append(surprise);
  const grid = el("div", "wp-grid");
  const more = el("button", "", "Show more");
  more.dataset.test = "wh-more"; more.hidden = true;
  root.append(bar, note, message, grid, more);
  wallhaven = { root, page: 0, last: 0, busy: false, using: null };
  const thumbs = new IntersectionObserver((entries) => entries.forEach((entry) => {
    if (!entry.isIntersecting) return;
    const tile = entry.target;
    thumbs.unobserve(tile);
    invoke("wallhaven_thumb", { id: tile.dataset.id }).then((url) => { if (url) tile.style.backgroundImage = "url(" + url + ")"; });
  }), { rootMargin: "300px" });
  const run = (page) => {
    if (wallhaven.busy) return;
    wallhaven.busy = true; message.textContent = ""; go.disabled = true; more.disabled = true;
    if (page === 1) {
      grid.replaceChildren(); more.hidden = true;
      for (let i = 0; i < 6; i++) grid.append(el("div", "wp-tile skeleton"));
    }
    invoke("wallhaven_search", { query: query.value, sorting: sorting.value, minimum: minimum.value, page }).then((results) => {
      grid.querySelectorAll(".skeleton").forEach((tile) => tile.remove());
      wallhaven.page = results.page; wallhaven.last = results.last_page;
      if (!results.hits.length && page === 1) message.textContent = "Nothing found. Try other words.";
      results.hits.forEach((hit) => {
        const tile = el("button", "wp-tile");
        tile.dataset.id = hit.id; tile.dataset.test = "wh-hit-" + hit.id;
        tile.title = hit.resolution + " · " + hit.size_mb;
        tile.append(el("span", "wp-name", hit.resolution + " · " + hit.size_mb));
        if (hit.saved) tile.append(el("span", "wp-saved", "Saved"));
        tile.onclick = () => {
          if (wallhaven.using) return;
          wallhaven.using = hit.id; tile.classList.add("busy"); message.textContent = "";
          invoke("wallhaven_use", { id: hit.id }).catch((error) => { message.textContent = String(error); })
            .then(() => { if (!tile.querySelector(".wp-saved")) tile.append(el("span", "wp-saved", "Saved")); })
            .finally(() => { wallhaven.using = null; tile.classList.remove("busy"); });
        };
        grid.append(tile);
        thumbs.observe(tile);
      });
      more.hidden = results.page >= results.last_page;
    }).catch((error) => { grid.querySelectorAll(".skeleton").forEach((tile) => tile.remove()); message.textContent = String(error); })
      .finally(() => { wallhaven.busy = false; go.disabled = false; more.disabled = false; queueReport(); });
  };
  bar.addEventListener("submit", (event) => { event.preventDefault(); run(1); });
  more.onclick = () => run(wallhaven.page + 1);
  surprise.onclick = () => { query.value = ""; sorting.value = "random"; run(1); };
  return root;
}

// Keyboard: the compositor's key binds, read only ----------------------------------------------------------------------------------
async function drawKeyboard() {
  const groups = await invoke("shortcuts_view");
  const host = $("keyboard-host");
  host.replaceChildren();
  const note = el("div", "setting-about note", "These are the key binds in your compositor config. Settings shows them and flags clashes; it does not change them.");
  host.append(note);
  groups.forEach((group) => {
    host.append(el("h4", "", group.name));
    const card = el("div", "rows-card");
    group.rows.forEach((row, index) => {
      const line = el("div", "setting-row");
      line.dataset.test = "shortcut:" + row.title;
      const text = el("div", "setting-text");
      text.append(el("div", "setting-title", row.title));
      if (row.conflict) text.append(el("div", "setting-about warn", "Also bound in another place. Only one of them will work."));
      const keys = el("div", "keycaps");
      row.keys.forEach((key) => keys.append(el("kbd", "", key)));
      line.append(text, keys);
      card.append(line);
    });
    host.append(card);
  });
  if (!groups.length) host.append(el("div", "empty", "No key binds found."));
  queueReport();
}

// Rows of service settings ----------------------------------------------------------------------------------------------------------
const pageRows = {};  // page name -> the rows last sent for it, kept so search can list matches from every page
function rowLine(page, row) {
  const line = el("div", "setting-row");
  const text = el("div", "setting-text");
  text.append(el("div", "setting-title", row.title));
  if (row.description) text.append(el("div", "setting-about", row.description));
  if (row.overridden) text.append(el("div", "setting-about", "Pinned by a local override."));
  line.append(text);
  const control = row.control;
  const send = (change) => invoke("setting_change", { page, id: row.id, change }).catch((error) => { console.error(error); invoke("ui_error", { text: row.id + ": " + String(error) }); });
  if (control.kind === "switch") {
    const box = el("input");
    box.type = "checkbox";
    box.className = "switch";
    box.checked = control.on;
    box.dataset.test = "set-" + row.id;
    box.addEventListener("change", () => send({ kind: "switch", value: box.checked }));
    line.append(box);
  } else if (control.kind === "slider") {
    const wrap = el("div", "slider-wrap");
    const range = el("input");
    range.type = "range";
    range.min = control.min;
    range.max = control.max;
    range.step = control.integer ? 1 : (control.max - control.min) / 100;
    range.value = control.value;
    range.dataset.test = "set-" + row.id;
    const format = (v) => (control.integer ? String(Math.round(v)) : Number(v).toFixed(2));
    const show = () => {
      const v = Number(range.value);
      range.style.setProperty("--pct", ((v - control.min) / (control.max - control.min) * 100) + "%");
      return format(v);
    };
    // The number can be typed as well as dragged; the model refuses anything outside the setting's range.
    const readout = el("input", "slider-input");
    readout.type = "number";
    readout.min = control.min;
    readout.max = control.max;
    readout.step = range.step;
    readout.value = show();
    readout.dataset.test = "set-" + row.id + "-number";
    range.addEventListener("input", () => { readout.value = show(); });
    range.addEventListener("change", () => send({ kind: "number", value: Number(range.value) }));
    // Typing replaces the number: the whole value is selected when the field is entered (the click that entered it must not move the caret).
    let justEntered = false;
    readout.addEventListener("focus", () => { justEntered = true; readout.select(); });
    readout.addEventListener("mouseup", (event) => { if (justEntered) { event.preventDefault(); justEntered = false; } });
    readout.addEventListener("keydown", (event) => { if (event.key === "Enter") readout.blur(); });   // Enter commits, like leaving the field
    readout.addEventListener("change", () => {
      const typed = Number(readout.value);
      if (readout.value === "" || !Number.isFinite(typed) || typed < control.min || typed > control.max) { readout.value = format(range.value); return; }
      range.value = typed;
      show();
      send({ kind: "number", value: typed });
    });
    wrap.append(range, readout);
    line.append(wrap);
  } else if (control.kind === "choice") {
    const segments = el("div", "segments");
    control.options.forEach((option) => {
      const button = el("button", "segment" + (option.value === control.current ? " current" : ""), option.label);
      button.dataset.test = "set-" + row.id + "-" + option.value;
      button.onclick = () => send({ kind: "choice", value: option.value });
      segments.append(button);
    });
    line.append(segments);
  }
  return line;
}
// Rows under their group headings, each group in one rounded card.
function drawGroups(host, page, rows) {
  let group = null;
  let card = null;
  rows.forEach((row) => {
    if (row.group !== group) {
      group = row.group;
      host.append(el("h4", "", group));
      card = el("div", "rows-card");
      host.append(card);
    }
    card.append(rowLine(page, row));
  });
}
function drawRows(view) {
  pageRows[view.page] = view.rows;
  const host = $(view.page + "-rows");
  host.replaceChildren();
  drawGroups(host, view.page, view.rows);
  if (!view.rows.length) host.append(el("div", "empty", "There is nothing to adjust here on this desktop."));
  if (searching()) drawSearch();
  queueReport();
}
listen("rows-view", (event) => drawRows(event.payload)).then(() => invoke("rows_view").then((pages) => pages.forEach(drawRows)));

// Search: matching rows from every page, under the page they belong to ------------------------------------------------------------
const searching = () => $("search").value.trim() !== "";
function drawSearch() {
  const words = $("search").value.toLowerCase().split(/\s+/).filter(Boolean);
  const host = $("search-rows");
  host.replaceChildren();
  let found = 0;
  PAGES.filter((page) => page.rows).forEach((page) => {
    const rows = (pageRows[page.name] || []).filter((row) => {
      const hay = (row.title + " " + row.group + " " + row.description).toLowerCase();
      return words.every((word) => hay.includes(word));
    });
    if (!rows.length) return;
    found += rows.length;
    host.append(el("h3", "search-page", page.title));
    drawGroups(host, page.name, rows);
  });
  if (!found) host.append(el("div", "empty", "No settings match."));
  queueReport();
}
$("search").addEventListener("input", () => {
  if (searching()) {
    document.querySelectorAll(".settings-page").forEach((node) => { node.hidden = node.id !== "page-search"; });
    document.querySelectorAll(".page-link").forEach((node) => node.classList.remove("current"));
    $("page-title").textContent = "Search";
    $("refresh").style.display = "none";
    drawSearch();
  } else {
    showPage(current);
  }
});

// Approvals: whether an approval may be given where the desktop cannot protect the dialog -----------------------------------------------
function drawApprovals(allow) {
  const card = $("approvals-card");
  card.replaceChildren();
  const line = el("div", "setting-row");
  const text = el("div", "setting-text");
  text.append(el("div", "setting-title", "Approve on a desktop that cannot protect the dialog"));
  text.append(el("div", "setting-about", allow
    ? "On. Other programs on this desktop might be able to click Allow for you or read the dialog. Actions that need confirmation are shown anyway."
    : "Off. Where the desktop cannot protect the approval dialog from other programs, actions that need confirmation are refused instead of shown."));
  const box = el("input");
  box.type = "checkbox";
  box.className = "switch";
  box.checked = allow;
  box.dataset.test = "approvals-unprotected";
  box.addEventListener("change", () => invoke("approvals_set", { allow: box.checked }).then(drawApprovals));
  line.append(text, box);
  card.append(line);
  queueReport();
}
invoke("approvals_view").then(drawApprovals);

// Privacy & agents ------------------------------------------------------------------------------------------------------------------
function drawAgents(view) {
  const note = $("agents-note");
  note.textContent = view.status === "connected" ? "" :
    view.status === "denied" ? "This copy of Settings is not the installed one, so it cannot manage agent access." :
    "The desktop service is not running.";
  note.style.display = view.status === "connected" ? "none" : "";
  const list = $("agents-clients");
  list.replaceChildren();
  view.clients.forEach((client, index) => {
    const card = el("div", "agent-card");
    card.dataset.test = "agent-" + index;
    const head = el("div", "agent-head");
    const who = el("div", "agent-who");
    who.append(el("div", "agent-name", client.name), el("div", "agent-identity", client.identity));
    const stats = el("div", "agent-stats", client.requests + " requests" + (client.denied ? " · " + client.denied + " refused" : "") + (client.last ? " · last " + client.last : ""));
    who.append(stats);
    const revoke = el("button", "danger", "Revoke");
    revoke.dataset.test = "revoke-" + index;
    revoke.onclick = () => invoke("agents_revoke", { client: client.id });
    head.append(who, revoke);
    card.append(head);
    client.tiers.forEach((tier) => {
      const row = el("label", "tier-row");
      const box = el("input");
      box.type = "checkbox";
      box.className = "switch";
      box.checked = tier.on;
      box.dataset.test = "tier-" + index + "-" + tier.name;
      box.addEventListener("change", () => invoke("agents_set_tier", { client: client.id, tier: tier.name, on: box.checked }));
      const text = el("div", "tier-text");
      text.append(el("div", "tier-name", tier.name), el("div", "tier-about", tier.about));
      row.append(box, text);
      card.append(row);
    });
    list.append(card);
  });
  if (!view.clients.length && view.status === "connected") list.append(el("div", "empty", "No application has been given access."));
  const history = $("agents-history");
  history.replaceChildren();
  view.history.forEach((row) => {
    const line = el("div", "hist-row");
    const main = el("div", "hist-main");
    main.append(el("div", "hist-what", row.tool + (row.args ? " " + row.args : "")),
      el("div", "hist-sub", row.who + " · " + row.time + " · " + row.tier + " · " + row.decision + (row.outcome ? " · " + row.outcome : "")));
    line.append(main);
    if (row.tainted) line.append(el("span", "hist-flag", "untrusted input"));
    history.append(line);
  });
  if (!view.history.length) history.append(el("div", "empty", "Nothing yet."));
  queueReport();
}
// Registering a listener takes a moment; anything sent meanwhile would be lost, so the current state is read once it is in place.
listen("agents-view", (event) => drawAgents(event.payload)).then(() => invoke("agents_view").then(drawAgents));

// Keys: Ctrl+F or / searches, Escape leaves the search, Ctrl+W closes, Alt+Up and Alt+Down move through the pages.
document.addEventListener("keydown", (event) => {
  const typing = ["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement && document.activeElement.tagName);
  if ((event.ctrlKey && event.key.toLowerCase() === "f") || (event.key === "/" && !typing)) {
    event.preventDefault();
    $("search").focus();
    $("search").select();
  } else if (event.key === "Escape" && $("search").value !== "") {
    $("search").value = "";
    $("search").dispatchEvent(new Event("input"));
    $("search").blur();
  } else if (event.ctrlKey && event.key.toLowerCase() === "w") {
    event.preventDefault();
    window.__TAURI__.window.getCurrentWindow().close();
  } else if (event.altKey && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
    event.preventDefault();
    const at = PAGES.findIndex((page) => page.name === current);
    const next = PAGES[(at + (event.key === "ArrowDown" ? 1 : PAGES.length - 1)) % PAGES.length];
    showPage(next.name);
  }
});

// Tests only: where every named control is, so a test can click it by name.
invoke("layout_reporting").then((wanted) => { reporting = wanted; queueReport(); });
function queueReport() {
  if (!reporting) return;
  clearTimeout(reportTimer);
  reportTimer = setTimeout(sendReport, 120);
}
function sendReport() {
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((node) => {
    const box = node.getBoundingClientRect();
    if (box.width > 0 && box.height > 0 && getComputedStyle(node).visibility !== "hidden") elements[node.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
  });
  invoke("report_layout", { report: { width: window.innerWidth, height: window.innerHeight, elements } });
}
window.addEventListener("resize", queueReport);
new MutationObserver(queueReport).observe(document.body, { childList: true, subtree: true });
