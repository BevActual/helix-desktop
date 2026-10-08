// One page hosts the bar and every panel. Rust decides what is open (shell-mode) and what the assistant shows (assistant-view);
// this file only draws it. Text from the assistant or the desktop is assigned with textContent only.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);



// The bar's right side is the system status, in the manner of a menu bar: Wi-Fi, Bluetooth, sound and battery, one button that opens
// the control center. The clock opens the notification shade. Everything here is drawn from the same views the panels use.
const statusState = { wifi: "off", bt: false };
function drawStatus() {
  const wifi = slot("wifi");
  const next = swapIcon(wifi, statusState.wifi === "off" ? "wifi-off" : "wifi");
  next.classList.toggle("dim", statusState.wifi !== "on");
  slot("bt").style.display = statusState.bt ? "" : "none";
}
let clockTimer = null;
function tickClock() {
  const now = new Date();
  $("qs-time").textContent = now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  $("qs-date").textContent = now.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" });
  document.querySelector("#clock .day").textContent = now.toLocaleDateString([], { weekday: "short", month: "short", day: "numeric" });
  document.querySelector("#clock .time").textContent = now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
}
// One timer, aligned to the minute: it moves the clock and re-reads the battery. Nothing else in the bar runs on a timer.
function everyMinute() {
  tickClock();
  invoke("battery_view").then(drawBattery);
  clearTimeout(clockTimer);
  clockTimer = setTimeout(everyMinute, 60000 - (Date.now() % 60000) + 50);
}

function show(view) {
  const phase = view.phase;
  let body = "";
  let reason = "";
  let note = "";
  if (phase === "thinking") body = "Thinking…";
  else if (phase === "acting") body = "Doing it…";
  else if (phase === "confirm") {
    body = "About to: " + view.description;
    if (view.reason) reason = "The assistant says: “" + view.reason + "”";
  } else if (phase === "result") {
    body = view.text || "Done.";
    if (view.unverified) note = "This came from outside the assistant.";
  } else if (view.failed) body = view.text;
  $("body").textContent = body;
  $("reason").textContent = reason;
  $("note").textContent = note;
  $("buttons").style.display = phase === "confirm" ? "flex" : "none";
  $("doit").disabled = phase !== "confirm" || !view.armed;
  if (phase === "idle" || phase === "result") $("q").focus();
}

listen("live-value", (event) => {
  $("live").textContent = String(event.payload); // kept for tests and logs; not shown
});
listen("assistant-view", (event) => show(event.payload));
listen("shell-mode", (event) => {
  const panel = event.payload.panel;
  document.body.classList.toggle("open", panel !== null);
  if (panel) document.body.dataset.panel = panel;
  else delete document.body.dataset.panel;
  if (panel === "assistant") {
    $("q").value = "";
    $("q").focus();
  } else if (panel === "control-center") {
    showDetail(null);
    tickClock();
    invoke("audio_view").then(drawAudio);
    invoke("network_view").then(drawNetwork);
    invoke("bluetooth_view").then(drawBluetooth);
    invoke("quick_view").then(drawQuick);
    invoke("brightness_view").then(drawBrightness);
    invoke("battery_view").then(drawBattery);
  } else if (panel === "quick-insert") {
    qiSource = "emoji"; // always opens on emoji, so the same keys do the same thing every time
    $("qi-input").value = "";
    qiSearch();
    $("qi-input").focus();
  } else if (panel === "agents") {
    invoke("activity_view").then(drawAgents);
  } else if (panel === "launcher") {
    $("lq").value = "";
    search();
    $("lq").focus();
  }
});

// The launcher. Rust returns display-safe names for application ids; this page never holds a command line and launching sends
// only an id. Results are drawn with textContent.
let hits = [];
let selected = 0;
let searchSeq = 0;
function drawResults() {
  const list = $("results");
  list.replaceChildren();
  hits.forEach((hit, index) => {
    const row = document.createElement("div");
    row.className = "row" + (index === selected ? " selected" : "");
    // The icon is a data: image URI built by Rust from a PNG or SVG file; an image element never runs script.
    const icon = document.createElement("img");
    icon.className = "icon";
    icon.alt = "";
    if (hit.icon) icon.src = hit.icon;
    row.append(icon);
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = hit.name;
    row.append(name);
    if (hit.generic) {
      const generic = document.createElement("span");
      generic.className = "generic";
      generic.textContent = hit.generic;
      row.append(generic);
    }
    row.addEventListener("click", () => launch(index));
    list.append(row);
  });
  $("empty").textContent = hits.length || !$("lq").value.trim() ? "" : "No matching applications";
}
function search() {
  const seq = ++searchSeq;
  invoke("launcher_query", { query: $("lq").value }).then((found) => {
    if (seq !== searchSeq) return; // a newer keystroke already asked
    hits = found;
    selected = 0;
    drawResults();
  });
}
function launch(index) {
  const hit = hits[index];
  if (hit) invoke("launcher_launch", { id: hit.id });
}
$("lq").addEventListener("input", search);
$("lq").addEventListener("keydown", (event) => {
  if (event.key === "ArrowDown" || event.key === "ArrowUp") {
    event.preventDefault();
    if (!hits.length) return;
    selected = (selected + (event.key === "ArrowDown" ? 1 : hits.length - 1)) % hits.length;
    drawResults();
  } else if (event.key === "Enter") {
    event.preventDefault();
    launch(selected);
  }
});
// Notifications. Applications write this text, so it is only ever assigned with textContent, and Rust has already reduced it to
// plain bounded text. Toasts never take the pointer; buttons live in the shade and send an index into the list Rust offered.
function ntext(cls, text) {
  const element = document.createElement("div");
  element.className = cls;
  element.textContent = text;
  return element;
}
// A coloured tile with the application's first letter, the colour taken from its name: a stable, quiet stand-in for an app icon.
function appBadge(name) {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + ch.codePointAt(0)) >>> 0;
  const badge = document.createElement("div");
  badge.className = "nico";
  badge.style.background = "hsl(" + (hash % 360) + " 52% 46%)";
  badge.textContent = Array.from(name)[0] || "•";
  return badge;
}
function notificationBody(item) {
  const text = document.createElement("div");
  text.className = "ntext";
  text.append(ntext("napp", item.app), ntext("nsum", item.summary));
  if (item.body) text.append(ntext("nbody", item.body));
  return text;
}
function drawNotifications(view) {
  const toasts = $("toasts");
  toasts.replaceChildren();
  view.toasts.forEach((item) => {
    const toast = document.createElement("div");
    toast.className = "toast" + (item.urgency === "critical" ? " critical" : "");
    toast.append(appBadge(item.app), notificationBody(item));
    toasts.append(toast);
  });
  const list = $("notif-list");
  list.replaceChildren();
  view.items.forEach((item, index) => {
    const row = document.createElement("div");
    row.className = "nrow" + (item.urgency === "critical" ? " critical" : "") + (item.live ? "" : " stale");
    const text = notificationBody(item);
    const acts = document.createElement("div");
    acts.className = "nacts";
    item.actions.forEach((action, j) => {
      acts.append(actionButton(action.label, "notif-" + index + "-action-" + j, () => invoke("notification_invoke", { id: item.id, revision: item.revision, index: j })));
    });
    if (item.actions.length) text.append(acts);
    // Dismiss is a quiet corner button that appears when the row is pointed at, as in a notification centre.
    const dismiss = document.createElement("button");
    dismiss.className = "ndismiss";
    dismiss.title = "Dismiss";
    dismiss.setAttribute("aria-label", "Dismiss");
    dismiss.dataset.test = "notif-" + index + "-dismiss";
    dismiss.append(icon("x"));
    dismiss.onclick = () => invoke("notification_dismiss", { id: item.id });
    row.append(appBadge(item.app), text, dismiss);
    list.append(row);
  });
  $("notif-empty").textContent = view.items.length ? "" : "No notifications";
  $("notif-clear").disabled = !view.items.length;
  $("notif-dnd").textContent = "Do not disturb";
  $("notif-dnd").classList.toggle("on", view.dnd);
  $("tile-dnd").classList.toggle("on", view.dnd);
  $("tile-dnd").dataset.on = view.dnd ? "1" : "0";
  $("tile-dnd-sub").textContent = view.dnd ? "On" : "Off";
  swapIcon(slot("bell"), view.dnd ? "moon" : "bell");
  $("notif-badge").hidden = !view.unread;
  $("notif-badge").textContent = view.unread > 9 ? "9+" : String(view.unread);
  queueReport();
}
listen("notifications-view", (event) => drawNotifications(event.payload));
invoke("notifications_view").then(drawNotifications);
$("notifbtn").onclick = () => invoke("panel_open", { id: "notifications" });
$("notif-clear").onclick = () => invoke("notification_clear");
$("notif-dnd").onclick = () => invoke("notification_set_dnd", { on: $("tile-dnd").dataset.on !== "1" });
$("tile-dnd").addEventListener("click", () => invoke("notification_set_dnd", { on: $("tile-dnd").dataset.on !== "1" }));


// The Caps Lock palette. Rust holds the emoji, the clipboard history and what each row means; this page draws display rows and sends
// back the position of the one chosen, so it can neither invent text to insert nor name an action it was not offered.
let qiSource = "emoji";
let qiRows = [];
let qiSelected = 0;
let qiSeq = 0;
let qiGeneration = 0;
const QI_COLUMNS = 8;
function qiDraw() {
  const list = $("qi-results");
  list.replaceChildren();
  const grid = qiSource === "emoji";
  list.classList.toggle("grid", grid);
  qiRows.forEach((row, index) => {
    const selected = index === qiSelected;
    let element;
    if (grid && row.glyph) {
      element = document.createElement("div");
      element.className = "qi-cell" + (selected ? " selected" : "");
      element.textContent = row.glyph;
      element.title = row.label;
    } else {
      element = document.createElement("div");
      element.className = "qi-row" + (selected ? " selected" : "") + (row.ask ? " ask" : "");
      if (row.ask) element.append(icon("sparkle"));
      if (row.glyph) { const g = document.createElement("span"); g.textContent = row.glyph; element.append(g); }
      const main = document.createElement("div");
      main.className = "qi-main";
      const label = document.createElement("div");
      label.className = "qi-label";
      label.textContent = row.label;
      main.append(label);
      element.append(main);
      const detail = document.createElement("div");
      detail.className = "qi-detail";
      detail.textContent = row.detail;
      element.append(detail);
    }
    element.dataset.test = "qi-row-" + index;
    element.addEventListener("click", () => qiChoose(index));
    list.append(element);
    if (selected) element.scrollIntoView({ block: "nearest" });
  });
  document.body.dataset.qi = qiSource;
  document.querySelectorAll(".qi-tab").forEach((tab) => tab.classList.toggle("current", tab.dataset.source === qiSource));
  $("qi-empty").textContent = qiRows.length ? "" : qiSource === "clipboard" ? "Nothing copied yet" : "No matches";
  queueReport();
}
function qiSearch() {
  const seq = ++qiSeq;
  invoke("quick_insert_query", { source: qiSource, text: $("qi-input").value }).then((results) => {
    if (seq !== qiSeq) return; // a newer keystroke already asked
    qiRows = results.rows;
    qiGeneration = results.generation;
    qiSelected = 0;
    qiDraw();
  });
}
function qiChoose(index) {
  // Rust only honours the choice for the results it was drawn from; if newer results exist, the press is ignored and can be repeated.
  if (index >= 0 && index < qiRows.length) invoke("quick_insert_activate", { generation: qiGeneration, index }).catch(() => qiSearch());
}
function qiSwitch(step) {
  const order = ["emoji", "clipboard", "actions"];
  qiSource = order[(order.indexOf(qiSource) + step + order.length) % order.length];
  qiSearch();
}
$("qi-input").addEventListener("input", qiSearch);
$("qi-input").addEventListener("keydown", (event) => {
  const grid = qiSource === "emoji";
  const move = (delta) => {
    event.preventDefault();
    if (!qiRows.length) return;
    qiSelected = Math.max(0, Math.min(qiRows.length - 1, qiSelected + delta));
    qiDraw();
  };
  if (event.key === "ArrowDown") move(grid ? QI_COLUMNS : 1);
  else if (event.key === "ArrowUp") move(grid ? -QI_COLUMNS : -1);
  else if (event.key === "ArrowRight" && grid) move(1);
  else if (event.key === "ArrowLeft" && grid) move(-1);
  else if (event.key === "Tab") { event.preventDefault(); qiSwitch(event.shiftKey ? -1 : 1); }
  else if (event.key === "Enter") { event.preventDefault(); qiChoose(qiSelected); }
});
document.querySelectorAll(".qi-tab").forEach((tab) => tab.addEventListener("click", () => { qiSource = tab.dataset.source; qiSearch(); $("qi-input").focus(); }));
$("qi-forget").onclick = () => invoke("quick_insert_forget_clipboard").then(qiSearch);

// Agents. Rust owns the picture (everything an agent can influence is sanitized there) and builds every request; this page only
// draws it and sends back ids it was given. Text is assigned with textContent only.
function arow(what, sub, flag) {
  const row = document.createElement("div");
  row.className = "arow";
  const main = document.createElement("div");
  main.className = "main";
  const title = document.createElement("div");
  title.className = "what";
  title.textContent = what;
  main.append(title);
  if (sub) {
    const line = document.createElement("div");
    line.className = "sub";
    line.textContent = sub;
    main.append(line);
  }
  row.append(main);
  if (flag) {
    const mark = document.createElement("span");
    mark.className = "flag";
    mark.textContent = flag;
    row.append(mark);
  }
  return row;
}
function actionButton(label, test, run) {
  const button = document.createElement("button");
  button.textContent = label;
  button.dataset.test = test;
  button.onclick = run;
  return button;
}
function drawAgents(view) {
  const bar = $("agentsbtn");
  bar.style.display = view.available ? "" : "none";
  bar.classList.toggle("busy", view.available && view.enabled && view.busy);
  bar.classList.toggle("off", view.available && !view.enabled);
  bar.title = !view.enabled ? "Agents are off" : view.busy ? view.pending.length + " waiting for you" : "Agents";
  $("agent-state").textContent = !view.available
    ? "Agent activity is shown by the registered shell."
    : view.enabled ? "Agents may ask to act. Nothing runs without the checks in Settings." : "Agents are off. Every request is refused until you turn them on.";
  $("agent-toggle").textContent = view.enabled ? "Turn off" : "Turn on";
  $("agent-toggle").disabled = !view.available;
  $("agent-stopall").disabled = !view.available || !view.busy;
  const pending = $("agent-pending");
  pending.replaceChildren();
  view.pending.forEach((row, index) => {
    const line = arow(row.who + " · " + (row.grant ? "wants access" : row.tool), row.reason, row.tainted ? "untrusted input" : "");
    line.append(actionButton("Stop", "stop-" + index, () => invoke("activity_stop", { operation: row.operation })));
    pending.append(line);
  });
  if (!view.pending.length) pending.append(arow("Nothing is waiting.", "", ""));
  const grants = $("agent-grants");
  grants.replaceChildren();
  view.grants.forEach((row, index) => {
    const line = arow(row.who, row.tiers.length ? row.tiers.join(", ") : "no access", "");
    line.append(actionButton("Revoke", "revoke-" + index, () => invoke("activity_revoke", { id: row.id })));
    grants.append(line);
  });
  if (!view.grants.length) grants.append(arow("No agent has access.", "", ""));
  const history = $("agent-history");
  history.replaceChildren();
  view.history.forEach((row) => {
    const what = row.tool + (row.args ? " " + row.args : "");
    history.append(arow(what, row.who + " · " + row.time + " · " + row.tier + " · " + row.decision + (row.outcome ? " · " + row.outcome : ""), row.tainted ? "untrusted input" : ""));
  });
  if (!view.history.length) history.append(arow("Nothing yet.", "", ""));
  queueReport();
}
listen("activity-view", (event) => drawAgents(event.payload));
invoke("activity_view").then(drawAgents);
$("agentsbtn").onclick = () => invoke("panel_open", { id: "agents" });
$("agent-stopall").onclick = () => invoke("activity_stop_all");
$("agent-toggle").onclick = () => invoke("activity_set_enabled", { enabled: $("agent-toggle").textContent === "Turn on" });
$("apps").onclick = () => invoke("panel_open", { id: "launcher" });
$("clock").onclick = () => invoke("panel_open", { id: "notifications" });
$("controlbtn").onclick = () => invoke("panel_open", { id: "control-center" });

// Sound. Rust owns the audio state and every write; the page sends "volume 40", "mute" or "make this output the default" and
// never names a device it was not given. Names are drawn with textContent.
let dragging = false;
let sendTimer = null;
let latest = 0;
function drawAudio(view) {
  const usable = view.available && view.output;
  $("audio-note").textContent = view.available ? (view.output ? "" : "No output device") : "Sound is unavailable";
  $("volrow").style.display = usable ? "" : "none";
  slot("vol").style.display = usable ? "" : "none";
  if (!usable) { queueReport(); return; }
  $("outputdev").textContent = view.output.name;
  if (!dragging) {
    $("volume").value = Math.min(view.percent, 100);
    $("percent").textContent = view.percent + "%";
  }
  swapIcon(slot("mute"), view.muted ? "volume-mute" : "volume");
  swapIcon(slot("vol"), view.muted ? "volume-mute" : "volume");
  $("volslider").style.setProperty("--pct", Math.min(view.percent, 100) + "%");
  $("mute").title = view.muted ? "Unmute" : "Mute";
  $("mute").dataset.muted = view.muted ? "1" : "0";
  const list = $("outputs");
  list.replaceChildren();
  view.outputs.forEach((output) => {
    const row = document.createElement("div");
    row.className = "row" + (output.id === view.output.id ? " current" : "");
    row.textContent = (output.id === view.output.id ? "● " : "○ ") + output.name;
    row.dataset.test = "out:" + output.name;
    row.addEventListener("click", () => invoke("audio_set_default", { id: output.id }));
    list.append(row);
  });
  queueReport();
}
listen("audio-view", (event) => drawAudio(event.payload));
// A drag sends at most one write every 50 ms, and always the last value, so the service is not flooded and the end state is right.
function sendVolume() {
  sendTimer = null;
  invoke("audio_set_volume", { percent: latest });
}
$("volume").addEventListener("pointerdown", () => { dragging = true; });
window.addEventListener("pointerup", () => { dragging = false; });
$("volume").addEventListener("input", () => {
  latest = Number($("volume").value);
  $("percent").textContent = latest + "%";
  $("volslider").style.setProperty("--pct", latest + "%");
  if (!sendTimer) sendTimer = setTimeout(sendVolume, 50);
});
// Wi-Fi. Same rule as sound: Rust holds the state and builds every request. A network is named by the path it was listed with, and
// Rust refuses one that is not in its last snapshot, or that needs a password (not supported yet), instead of sending it.
const bars = (strength) => (strength >= 75 ? "▂▄▆█" : strength >= 50 ? "▂▄▆" : strength >= 25 ? "▂▄" : "▂");
function drawNetwork(view) {
  statusState.wifi = !view.available || !view.wifi_enabled ? "off" : view.connected ? "on" : "idle";
  drawStatus();
  $("tile-wifi").style.display = view.available ? "" : "none";
  if (!view.available) return;
  $("tile-wifi").classList.toggle("on", view.wifi_enabled);
  $("tile-wifi").dataset.on = view.wifi_enabled ? "1" : "0";
  $("tile-wifi-sub").textContent = view.wifi_enabled ? view.status.replace(/^Connected to /, "") : "Off";
  $("wifi-status").textContent = view.status;
  $("wifitoggle").textContent = view.wifi_enabled ? "Turn off" : "Turn on";
  $("wifitoggle").dataset.on = view.wifi_enabled ? "1" : "0";
  $("scan").style.display = view.wifi_enabled ? "" : "none";
  $("scan").disabled = view.scanning;
  $("disconnect").style.display = view.connected ? "" : "none";
  const list = $("networks");
  list.replaceChildren();
  view.networks.forEach((net) => {
    const row = document.createElement("div");
    const joinable = net.action === "connect";
    row.className = "net" + (net.active ? " active" : "") + (joinable ? " clickable" : "") + (joinable || net.active ? "" : " unavailable");
    row.dataset.test = "net:" + net.ssid;
    const signal = document.createElement("span");
    signal.className = "bars";
    signal.textContent = bars(net.strength);
    const name = document.createElement("span");
    name.className = "ssid";
    name.textContent = net.ssid;
    row.append(signal, name);
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = net.active ? "Connected"
      : net.action === "password" ? "Needs a password"
      : net.action === "enterprise" ? "Enterprise" : net.saved ? "Saved" : net.secured ? "Secured" : "Open";
    row.append(tag);
    if (joinable) row.addEventListener("click", () => invoke("network_connect", { path: net.path }));
    if (net.saved) {
      const forget = document.createElement("button");
      forget.textContent = "Forget";
      forget.dataset.test = "net-forget:" + net.ssid;
      forget.addEventListener("click", (event) => { event.stopPropagation(); invoke("network_forget", { path: net.path }); });
      row.append(forget);
    }
    list.append(row);
  });
  const vpns = $("vpns");
  vpns.replaceChildren();
  view.vpns.forEach((vpn) => {
    const row = document.createElement("div");
    row.className = "net clickable" + (vpn.active ? " active" : "");
    row.dataset.test = "vpn:" + vpn.name;
    const name = document.createElement("span");
    name.className = "ssid";
    name.textContent = "VPN: " + vpn.name;
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = vpn.active ? "On" : "Off";
    row.append(name, tag);
    row.addEventListener("click", () => invoke("network_vpn", { path: vpn.path, active: !vpn.active }));
    vpns.append(row);
  });
}
listen("network-view", (event) => { drawNetwork(event.payload); queueReport(); });
$("wifitoggle").onclick = () => invoke("network_set_wifi", { enabled: $("wifitoggle").dataset.on !== "1" });
$("scan").onclick = () => invoke("network_scan");
$("disconnect").onclick = () => invoke("network_disconnect");

// Bluetooth. Same rules: Rust holds the state and builds every request; a device is named by the path it was listed with. Pairing a new
// device is not built yet, so a device that is not paired is listed with a note and cannot be clicked.
function drawBluetooth(view) {
  statusState.bt = view.available && view.enabled;
  drawStatus();
  $("tile-bt").style.display = view.available ? "" : "none";
  if (!view.available) return;
  $("tile-bt").classList.toggle("on", view.enabled);
  $("tile-bt").dataset.on = view.enabled ? "1" : "0";
  $("tile-bt").dataset.blocked = view.blocked ? "1" : "0";
  $("tile-bt-sub").textContent = view.blocked ? "Blocked" : view.enabled ? view.status.replace(/^Connected to /, "") : "Off";
  $("bt-status").textContent = view.status;
  $("bttoggle").textContent = view.enabled ? "Turn off" : "Turn on";
  $("bttoggle").dataset.on = view.enabled ? "1" : "0";
  $("bttoggle").disabled = view.blocked;
  $("btscan").style.display = view.enabled ? "" : "none";
  $("btscan").textContent = view.scanning ? "Stop" : "Scan";
  $("btscan").dataset.scanning = view.scanning ? "1" : "0";
  const list = $("btdevices");
  list.replaceChildren();
  view.devices.forEach((device) => {
    const row = document.createElement("div");
    const usable = device.action === "connect" || device.action === "disconnect";
    row.className = "net" + (device.connected ? " active" : "") + (usable ? " clickable" : "") + (usable || device.connected ? "" : " unavailable");
    row.dataset.test = "bt:" + device.name;
    const name = document.createElement("span");
    name.className = "ssid";
    name.textContent = device.name;
    row.append(name);
    const tag = document.createElement("span");
    tag.className = "tag";
    const battery = device.battery === null || device.battery === undefined ? "" : " · " + device.battery + "%";
    if (device.action === "pair") tag.title = "Pairing a new device is not available yet";
    tag.textContent = (device.action === "pair" ? "Not paired"
      : device.action === "busy" ? "Connecting…" : device.connected ? "Connected" : "Paired") + battery;
    row.append(tag);
    if (device.action === "connect") row.addEventListener("click", () => invoke("bluetooth_connect", { path: device.path }));
    if (device.action === "disconnect") row.addEventListener("click", () => invoke("bluetooth_disconnect", { path: device.path }));
    if (device.paired) {
      const forget = document.createElement("button");
      forget.textContent = "Forget";
      forget.dataset.test = "bt-forget:" + device.name;
      forget.addEventListener("click", (event) => { event.stopPropagation(); invoke("bluetooth_forget", { path: device.path }); });
      row.append(forget);
    }
    list.append(row);
  });
}
listen("bluetooth-view", (event) => { drawBluetooth(event.payload); queueReport(); });
$("bttoggle").onclick = () => invoke("bluetooth_set_power", { enabled: $("bttoggle").dataset.on !== "1" });
$("btscan").onclick = () => invoke("bluetooth_scan", { on: $("btscan").dataset.scanning !== "1" });

$("mute").onclick = () => invoke("audio_set_mute", { muted: $("mute").dataset.muted !== "1" });

$("ask").onclick = () => invoke("panel_open", { id: "assistant" });
$("scrim").addEventListener("click", () => invoke("panel_outside"));
$("q").addEventListener("keydown", (event) => {
  if (event.key !== "Enter") return;
  event.preventDefault();
  const request = $("q").value;
  $("q").value = "";
  invoke("assistant_submit", { request }).then(show);
});
$("cancel").onclick = () => invoke("assistant_cancel").then(show);
$("doit").onclick = () => invoke("assistant_confirm").then(show);
document.addEventListener("keydown", (event) => {
  if (event.key !== "Escape") return;
  if (document.body.dataset.panel === "control-center" && detail) { showDetail(null); return; }
  invoke("panel_escape");
});

// The tiles. A tile's body switches the thing it stands for; the arrow opens its details. Rust owns the state and builds every request.
$("tile-wifi").addEventListener("click", (event) => {
  if (event.target.closest(".tile-open")) return;
  invoke("network_set_wifi", { enabled: $("tile-wifi").dataset.on !== "1" });
});
$("tile-bt").addEventListener("click", (event) => {
  if (event.target.closest(".tile-open") || $("tile-bt").dataset.blocked === "1") return;
  invoke("bluetooth_set_power", { enabled: $("tile-bt").dataset.on !== "1" });
});
$("tile-dark").addEventListener("click", () => invoke("quick_set_dark", { on: $("tile-dark").dataset.on !== "1" }));
$("tile-night").addEventListener("click", () => invoke("quick_set_night_light", { on: $("tile-night").dataset.on !== "1" }));
function drawQuick(view) {
  // The shell follows the system theme: the same setting the Dark theme tile changes.
  document.documentElement.dataset.theme = view.dark === false ? "light" : "dark";
  const dark = view.dark !== null && view.dark !== undefined;
  $("tile-dark").style.display = dark ? "" : "none";
  if (dark) {
    $("tile-dark").classList.toggle("on", view.dark);
    $("tile-dark").dataset.on = view.dark ? "1" : "0";
    $("tile-dark-sub").textContent = view.dark_label;
  }
  const night = view.night_light !== null && view.night_light !== undefined;
  $("tile-night").style.display = night ? "" : "none";
  if (night) {
    $("tile-night").classList.toggle("on", view.night_light);
    $("tile-night").dataset.on = view.night_light ? "1" : "0";
    $("tile-night-sub").textContent = view.night_light ? "On" : "Off";
  }
  queueReport();
}
listen("quick-view", (event) => drawQuick(event.payload));

function drawBattery(view) {
  $("qs-battery").style.display = view.present ? "" : "none";
  $("batt").style.display = view.present ? "" : "none";
  if (!view.present) return;
  $("qs-battery").textContent = view.label + (view.state ? " · " + view.state : "");
  $("qs-battery").classList.toggle("low", view.low);
  $("batt-label").textContent = view.label;
  $("batt").classList.toggle("low", view.low);
  $("batt").classList.toggle("charging", view.charging);
  // The glyph's fill is the charge: one rectangle inside the outline, drawn here so it needs no markup parsing.
  const glyph = slot("batt");
  glyph.querySelectorAll(".level").forEach((old) => old.remove());
  const level = document.createElementNS(SVG, "rect");
  level.setAttribute("class", "level solid");
  level.setAttribute("x", 4.4); level.setAttribute("y", 9.4); level.setAttribute("height", 5.2); level.setAttribute("rx", 1.1);
  level.setAttribute("width", Math.max(1.2, 12.2 * Math.min(view.percent, 100) / 100));
  glyph.append(level);
  queueReport();
}
listen("battery-view", (event) => drawBattery(event.payload));


// Details. One page at a time inside the panel, with a back arrow; Escape goes back before it closes the panel.
let detail = null;
const titles = { sound: "Sound output", wifi: "Internet", bluetooth: "Bluetooth" };
function showDetail(name) {
  detail = name;
  $("qs-main").style.display = name ? "none" : "";
  $("qs-detail").style.display = name ? "" : "none";
  $("detail-sound").classList.toggle("current", name === "sound");
  $("wifi").classList.toggle("current", name === "wifi");
  $("bluetooth").classList.toggle("current", name === "bluetooth");
  $("detail-title").textContent = titles[name] || "";
  queueReport();
}
$("tile-wifi-open").onclick = () => showDetail("wifi");
$("tile-bt-open").onclick = () => showDetail("bluetooth");
$("sound-open").onclick = () => showDetail("sound");
$("back").onclick = () => showDetail(null);



// The on-screen indicator for the volume and brightness keys: Rust says what changed and to what; this draws it for a moment.
let osdTimer = null;
listen("osd", (event) => {
  const { kind, percent, muted, text } = event.payload;
  const osd = $("osd");
  osd.classList.toggle("notice", kind === "notice");
  if (kind === "notice") {
    swapIcon(slot("osd-ico"), "shield");
    $("osd-text").textContent = text || "";
  } else {
    swapIcon(slot("osd-ico"), kind === "brightness" ? "sun" : muted || percent === 0 ? "volume-mute" : "volume");
    osd.style.setProperty("--pct", percent + "%");
    osd.classList.toggle("muted", !!muted);
    $("osd-val").textContent = muted ? "Muted" : percent + "%";
  }
  osd.classList.add("show");
  clearTimeout(osdTimer);
  osdTimer = setTimeout(() => osd.classList.remove("show"), kind === "notice" ? 3200 : 1500);
});
new MutationObserver(() => queueGlass()).observe($("osd"), { attributes: true, attributeFilter: ["class"] });

// Brightness. Rust reads the backlight and clamps every write (never black); the slider sends at most one write every 50 ms.
let brightTimer = null;
let brightLatest = 0;
let brightDragging = false;
function drawBrightness(view) {
  $("brightrow").style.display = view.available ? "" : "none";
  if (!view.available || brightDragging) { queueReport(); return; }
  $("brightness").value = view.percent;
  $("brightslider").style.setProperty("--pct", view.percent + "%");
  $("bright-percent").textContent = view.percent + "%";
  queueReport();
}
listen("brightness-view", (event) => drawBrightness(event.payload));
$("brightness").addEventListener("pointerdown", () => { brightDragging = true; });
window.addEventListener("pointerup", () => { brightDragging = false; });
$("brightness").addEventListener("input", () => {
  brightLatest = Number($("brightness").value);
  $("bright-percent").textContent = brightLatest + "%";
  $("brightslider").style.setProperty("--pct", brightLatest + "%");
  if (!brightTimer) brightTimer = setTimeout(() => { brightTimer = null; invoke("brightness_set", { percent: brightLatest }); }, 50);
});



// Workspaces. Rust sends the strip for the output being worked on; clicking sends only an id from that strip, and Rust refuses any other.
function drawWorkspaces(strip) {
  const host = $("workspaces");
  host.replaceChildren();
  host.style.display = strip.length > 1 ? "" : "none"; // a lone workspace is not worth a control
  strip.forEach((ws, index) => {
    // A roomy invisible button around a small pill: the pill is what is seen, the button is what is clicked.
    const pill = document.createElement("button");
    pill.className = "ws-hit";
    const mark = document.createElement("span");
    mark.className = "ws" + (ws.active ? " active" : "") + (ws.windows ? " has-windows" : "") + (ws.urgent ? " urgent" : "");
    pill.append(mark);
    pill.dataset.test = "ws-" + index;
    pill.title = ws.name ? ws.name : "Workspace " + ws.idx;
    pill.setAttribute("aria-label", pill.title);
    pill.onclick = () => invoke("workspace_focus", { id: ws.id });
    host.append(pill);
  });
  queueReport();
  queueGlass();
}
listen("workspaces-view", (event) => drawWorkspaces(event.payload));
invoke("workspaces_view").then(drawWorkspaces);


// The tray. Applications publish these icons and titles, so they are drawn as images (never as markup) and named by a key Rust made.
function trayIcon(item) {
  if (item.icon_uri) { const img = document.createElement("img"); img.src = item.icon_uri; img.alt = ""; img.draggable = false; return img; }
  if (item.pixmap) {
    const { width, height, rgba } = item.pixmap;
    const canvas = document.createElement("canvas");
    canvas.width = width; canvas.height = height;
    const bytes = Uint8ClampedArray.from(atob(rgba), (ch) => ch.charCodeAt(0));
    if (bytes.length === width * height * 4) canvas.getContext("2d").putImageData(new ImageData(bytes, width, height), 0, 0);
    return canvas;
  }
  return dockLetterTile(item.title || "?");
}

// An application's own menu. Rust reads it from the application and remembers exactly what it showed, so a choice is only passed on if it
// was an enabled entry of this menu; applications with no menu get the plain ContextMenu call instead.
let trayMenuOpen = false;
function closeTrayMenu() { trayMenuOpen = false; $("tray-menu").classList.remove("show"); queueInteractive(); queueGlass(); queueReport(); }
async function openTrayMenu(item, button, at) {
  let items = [];
  try { items = await invoke("tray_menu", { key: item.key }); } catch (error) { items = []; }
  if (!items.length) { invoke("tray_context", at); return; }
  const menu = $("tray-menu");
  menu.replaceChildren();
  const add = (entry, sub) => {
    if (entry.kind === "separator") { const sep = document.createElement("div"); sep.className = "menu-sep"; menu.append(sep); return; }
    const row = document.createElement("div");
    row.className = "menu-row" + (sub ? " sub" : "") + (entry.kind === "submenu" ? " header" : "") + (!entry.enabled && entry.kind !== "submenu" ? " disabled" : "");
    const tick = document.createElement("span");
    tick.className = "tick";
    tick.textContent = entry.checked ? (entry.kind === "radio" ? "●" : "✓") : "";
    const label = document.createElement("span");
    label.textContent = entry.label;
    row.append(tick, label);
    row.dataset.test = "tray-menu-" + entry.id;
    if (entry.kind !== "submenu") row.addEventListener("click", () => { if (!entry.enabled) return; closeTrayMenu(); invoke("tray_menu_activate", { key: item.key, id: entry.id }); });
    menu.append(row);
    (entry.children || []).forEach((child) => add(child, true));
  };
  items.forEach((entry) => add(entry, false));
  menu.classList.add("show");
  const box = button.getBoundingClientRect();
  menu.style.left = Math.max(8, Math.min(window.innerWidth - menu.offsetWidth - 8, box.right - menu.offsetWidth)) + "px";
  menu.style.top = box.bottom + 8 + "px";
  trayMenuOpen = true;
  queueInteractive();
  queueGlass();
  queueReport();
}
document.addEventListener("pointerdown", (event) => { if (trayMenuOpen && !event.target.closest("#tray-menu")) closeTrayMenu(); });
document.addEventListener("keydown", (event) => { if (event.key === "Escape" && trayMenuOpen) { event.stopPropagation(); closeTrayMenu(); } }, true);

function drawTray(items) {
  const host = $("tray");
  host.replaceChildren();
  items.forEach((item, index) => {
    const button = document.createElement("button");
    button.className = "tray-item" + (item.status === "attention" ? " attention" : "");
    button.dataset.test = "tray-" + index;
    button.title = item.title;
    button.setAttribute("aria-label", item.title);
    const icon = trayIcon(item);
    if (icon.classList.contains("dock-tile")) icon.classList.add("tray-tile");
    button.append(icon);
    const where = (event) => ({ key: item.key, x: Math.round(event.clientX), y: Math.round(event.clientY) });
    button.addEventListener("click", (event) => invoke("tray_activate", where(event)));
    button.addEventListener("auxclick", (event) => { if (event.button === 1) invoke("tray_secondary", where(event)); });
    button.addEventListener("contextmenu", (event) => { event.preventDefault(); openTrayMenu(item, button, where(event)); });
    host.append(button);
  });
  queueReport();
  queueGlass();
}
listen("tray-view", (event) => drawTray(event.payload));
invoke("tray_view").then(drawTray);

// The dock. Rust decides what is listed (pinned first, then whatever is open) and what a click does; this page draws it and sends back ids
// it was given. It stays hidden until the pointer touches the bottom edge, and while hidden only a thin strip there takes the pointer.
let dockItems = [];
let dockHideTimer = null;
let dockMenuOpen = false;
function dockLetterTile(name) {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + ch.codePointAt(0)) >>> 0;
  const tile = document.createElement("div");
  tile.className = "dock-tile";
  tile.style.background = "hsl(" + (hash % 360) + " 45% 42%)";
  tile.textContent = Array.from(name)[0] || "•";
  return tile;
}
function drawDock(items) {
  dockItems = items;
  const row = $("dock-items");
  row.replaceChildren();
  items.forEach((item, index) => {
    const cell = document.createElement("div");
    cell.className = "dock-item" + (item.running ? " running" : "") + (item.focused ? " focused" : "") + (item.urgent ? " urgent" : "");
    cell.dataset.test = "dock-item-" + index;
    cell.dataset.name = item.name;
    if (item.icon_uri) { const img = document.createElement("img"); img.src = item.icon_uri; img.alt = ""; img.draggable = false; cell.append(img); }
    else cell.append(dockLetterTile(item.name));
    cell.addEventListener("click", () => invoke("dock_activate", { id: item.id }));
    cell.addEventListener("auxclick", (event) => { if (event.button === 1) invoke("dock_new_window", { id: item.id }); });
    cell.addEventListener("contextmenu", (event) => { event.preventDefault(); openDockMenu(item, cell); });
    cell.addEventListener("pointerenter", () => showDockTip(item.name, cell));
    cell.addEventListener("pointerleave", () => $("dock-tip").classList.remove("show"));
    row.append(cell);
  });
  queueInteractive();
  queueGlass();
}
// The dock is centred with a transform, which offset boxes ignore; where it rests is simply centred above the bottom margin.
function dockBox() {
  const dock = $("dock");
  return { x: (window.innerWidth - dock.offsetWidth) / 2, y: window.innerHeight - 10 - dock.offsetHeight, w: dock.offsetWidth, h: dock.offsetHeight, radius: parseFloat(getComputedStyle(dock).borderTopLeftRadius) || 0 };
}
function showDockTip(name, cell) {
  const tip = $("dock-tip");
  tip.textContent = name;
  const box = cell.getBoundingClientRect();
  tip.style.left = box.left + box.width / 2 + "px";
  tip.style.top = box.top - 42 + "px";
  tip.classList.add("show");
}
function setDock(show) {
  document.body.classList.toggle("dock-show", show);
  if (!show) { $("dock-tip").classList.remove("show"); closeDockMenu(); }
  queueInteractive();
  queueGlass();
}
function scheduleDockHide() {
  clearTimeout(dockHideTimer);
  dockHideTimer = setTimeout(() => { if (!dockMenuOpen) setDock(false); }, 450);
}
$("dock-edge").addEventListener("pointerenter", () => { clearTimeout(dockHideTimer); if (!document.body.classList.contains("open")) setDock(true); });
$("dock").addEventListener("pointerenter", () => clearTimeout(dockHideTimer));
$("dock").addEventListener("pointerleave", () => { magnify(null); scheduleDockHide(); });
$("dock-edge").addEventListener("pointerleave", () => { if (!$("dock").matches(":hover")) scheduleDockHide(); });
// Icons grow toward the pointer: a smooth bump (a Gaussian of the distance) rather than steps.
function magnify(x) {
  document.querySelectorAll(".dock-item").forEach((cell) => {
    let scale = 1;
    if (x !== null) {
      const box = cell.getBoundingClientRect();
      const distance = x - (box.left + box.width / 2);
      scale = 1 + 0.62 * Math.exp(-(distance * distance) / (2 * 62 * 62));
    }
    cell.style.setProperty("--s", scale.toFixed(3));
  });
}
$("dock").addEventListener("pointermove", (event) => magnify(event.clientX));
function closeDockMenu() { dockMenuOpen = false; $("dock-menu").classList.remove("show"); queueInteractive(); }
function openDockMenu(item, cell) {
  const menu = $("dock-menu");
  menu.replaceChildren();
  const title = document.createElement("div");
  title.className = "dock-menu-title";
  title.textContent = item.name;
  menu.append(title);
  const entry = (label, test, run) => {
    const row = document.createElement("div");
    row.className = "dock-menu-item";
    row.textContent = label;
    row.dataset.test = test;
    row.addEventListener("click", () => { closeDockMenu(); run(); });
    menu.append(row);
  };
  entry(item.running ? "New window" : "Open", "dock-menu-new", () => invoke("dock_new_window", { id: item.id }));
  if (!item.id.startsWith("app:")) {
    if (item.pinned) entry("Remove from Dock", "dock-menu-unpin", () => invoke("dock_unpin", { id: item.id }));
    else entry("Keep in Dock", "dock-menu-pin", () => invoke("dock_pin", { id: item.id }));
  }
  menu.classList.add("show");
  const box = cell.getBoundingClientRect();
  menu.style.left = Math.max(8, box.left + box.width / 2 - 95) + "px";
  menu.style.top = Math.max(8, box.top - menu.offsetHeight - 14) + "px";
  dockMenuOpen = true;
  queueInteractive();
}
document.addEventListener("pointerdown", (event) => { if (dockMenuOpen && !event.target.closest("#dock-menu")) closeDockMenu(); });
listen("dock-view", (event) => drawDock(event.payload));
invoke("dock_view").then(drawDock);

// What takes the pointer while no panel is open besides the bar: the dock when it is showing (and its menu), else the strip that summons it.
let interactiveQueued = false;
let lastInteractive = "";
function reportInteractive() {
  interactiveQueued = false;
  const open = document.body.classList.contains("open");
  const rects = [];
  if (!open && (trayMenuOpen || dockMenuOpen)) {
    rects.push({ x: 0, y: 0, w: window.innerWidth, h: window.innerHeight, radius: 0 });
  } else if (!open) {
    if (document.body.classList.contains("dock-show")) {
      const dock = dockBox();
      rects.push({ x: dock.x - 12, y: dock.y - 12, w: dock.w + 24, h: dock.h + 24 + 12, radius: 0 });
      if (dockMenuOpen) rects.push({ ...layoutBox($("dock-menu")), radius: 0 });
    } else {
      rects.push({ ...layoutBox($("dock-edge")), radius: 0 });
    }
  }
  const key = JSON.stringify(rects) + window.innerWidth + "x" + window.innerHeight;
  if (key === lastInteractive) return;
  lastInteractive = key;
  invoke("report_interactive", { elements: rects, width: window.innerWidth, height: window.innerHeight });
}
function queueInteractive() {
  if (interactiveQueued) return;
  interactiveQueued = true;
  requestAnimationFrame(reportInteractive);
}
new MutationObserver(queueInteractive).observe(document.body, { attributes: true, attributeFilter: ["data-panel", "class"] });
window.addEventListener("resize", queueInteractive);
queueInteractive();

// Glass. The compositor blurs what is behind rectangles the shell names; the shell is one surface over the whole output, so what to blur
// is whatever is drawn as glass right now: the bar, the open panel and the toasts. Boxes come from the layout (offsets), not from the
// animated transform, so the blur is in place from the first frame of an opening panel.
const GLASS_PANEL = { "control-center": "controls", notifications: "notifs", agents: "agents", launcher: "launcher", assistant: "panel", "quick-insert": "quickinsert" };
function layoutBox(element) {
  let x = 0, y = 0;
  for (let node = element; node; node = node.offsetParent) { x += node.offsetLeft; y += node.offsetTop; }
  return { x, y, w: element.offsetWidth, h: element.offsetHeight, radius: parseFloat(getComputedStyle(element).borderTopLeftRadius) || 0 };
}
let lastGlass = "";
let glassQueued = false;
function reportGlass() {
  glassQueued = false;
  const elements = [$("bar")];
  if ($("osd").classList.contains("show")) elements.push($("osd"));
  if (document.body.classList.contains("dock-show") && !document.body.dataset.panel) elements.push(dockBox());
  if (dockMenuOpen) elements.push($("dock-menu"));
  if (trayMenuOpen) elements.push($("tray-menu"));
  const open = document.body.dataset.panel;
  if (open && GLASS_PANEL[open]) elements.push($(GLASS_PANEL[open]));
  document.querySelectorAll("#toasts .toast").forEach((toast) => elements.push(toast));
  if (open) elements.splice(0, elements.length, ...elements.filter((el) => !(el instanceof Element) || !el.classList.contains("toast"))); // toasts are hidden while a panel is open
  const boxes = elements.map((el) => (el instanceof Element ? layoutBox(el) : el));
  const key = JSON.stringify(boxes) + window.innerWidth + "x" + window.innerHeight;
  if (key === lastGlass) return;
  lastGlass = key;
  invoke("report_glass", { elements: boxes, width: window.innerWidth, height: window.innerHeight });
}
function queueGlass() {
  if (glassQueued) return;
  glassQueued = true;
  requestAnimationFrame(reportGlass);
}
listen("blur-active", (event) => { document.documentElement.dataset.blur = event.payload ? "1" : ""; });
new MutationObserver(queueGlass).observe(document.body, { attributes: true, attributeFilter: ["data-panel", "class"] });
new MutationObserver(queueGlass).observe($("toasts"), { childList: true });
const glassResize = new ResizeObserver(queueGlass);
["bar", "controls", "notifs", "agents", "launcher", "panel", "quickinsert", "toasts", "osd", "dock", "tray-menu"].forEach((id) => glassResize.observe($(id)));
window.addEventListener("resize", queueGlass);
queueGlass();

// Tests only: when Rust says a layout log is wanted, report where every named control is, so a test can click it by name.
let reporting = false;
let reportTimer = null;
invoke("layout_reporting").then((wanted) => { reporting = wanted; queueReport(); });
let reportDue = 0;
// `delay` is long only after a panel opened or closed (it animates for ~240 ms); anything else is reported almost at once. A later,
// shorter request never pulls a pending longer one earlier, so positions are never read mid-animation.
function queueReport(delay = 80) {
  if (!reporting) return;
  const due = Date.now() + delay;
  if (reportTimer && due < reportDue) return;
  clearTimeout(reportTimer);
  reportDue = due;
  reportTimer = setTimeout(() => { reportTimer = null; sendReport(); }, delay);
}
function sendReport() {
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((element) => {
    const box = element.getBoundingClientRect();
    // A closed panel is laid out but invisible (visibility is inherited): it has no controls to click.
    if (box.width > 0 && box.height > 0 && getComputedStyle(element).visibility !== "hidden") elements[element.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
  });
  invoke("report_layout", { report: { width: window.innerWidth, height: window.innerHeight, panel: document.body.dataset.panel || "", detail, elements } });
}
window.addEventListener("resize", () => queueReport());
new MutationObserver(() => queueReport(300)).observe(document.body, { attributes: true, attributeFilter: ["data-panel", "class"] });

invoke("quick_view").then(drawQuick);
invoke("brightness_view").then(drawBrightness);
invoke("audio_view").then(drawAudio);
invoke("network_view").then(drawNetwork);
invoke("bluetooth_view").then(drawBluetooth);
everyMinute();
requestAnimationFrame(() => invoke("ready"));
