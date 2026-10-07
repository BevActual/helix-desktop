// One page hosts the bar and every panel. Rust decides what is open (shell-mode) and what the assistant shows (assistant-view);
// this file only draws it. Text from the assistant or the desktop is assigned with textContent only.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);

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
  $("live").textContent = String(event.payload);
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
    invoke("battery_view").then(drawBattery);
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
  bar.textContent = !view.enabled ? "Agents off" : view.busy ? "Agents · " + view.pending.length + " waiting" : "Agents";
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
  if (!usable) { queueReport(); return; }
  $("outputdev").textContent = view.output.name;
  if (!dragging) {
    $("volume").value = Math.min(view.percent, 100);
    $("percent").textContent = view.percent + "%";
  }
  $("mute").textContent = view.muted ? "🔇" : "🔊";
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
  if (!sendTimer) sendTimer = setTimeout(sendVolume, 50);
});
// Wi-Fi. Same rule as sound: Rust holds the state and builds every request. A network is named by the path it was listed with, and
// Rust refuses one that is not in its last snapshot, or that needs a password (not supported yet), instead of sending it.
const bars = (strength) => (strength >= 75 ? "▂▄▆█" : strength >= 50 ? "▂▄▆" : strength >= 25 ? "▂▄" : "▂");
function drawNetwork(view) {
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
  if (!view.present) return;
  $("qs-battery").textContent = view.label + (view.state ? " · " + view.state : "");
  $("qs-battery").classList.toggle("low", view.low);
  queueReport();
}
listen("battery-view", (event) => drawBattery(event.payload));

function tickClock() {
  const now = new Date();
  $("qs-time").textContent = now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  $("qs-date").textContent = now.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" });
}
setInterval(() => { if (document.body.dataset.panel === "control-center") tickClock(); }, 20000);

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

// Tests only: when Rust says a layout log is wanted, report where every named control is, so a test can click it by name.
let reporting = false;
let reportTimer = null;
invoke("layout_reporting").then((wanted) => { reporting = wanted; queueReport(); });
function queueReport() {
  if (!reporting) return;
  clearTimeout(reportTimer);
  reportTimer = setTimeout(sendReport, 80);
}
function sendReport() {
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((element) => {
    const box = element.getBoundingClientRect();
    if (box.width > 0 && box.height > 0) elements[element.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
  });
  invoke("report_layout", { report: { width: window.innerWidth, height: window.innerHeight, panel: document.body.dataset.panel || "", detail, elements } });
}
window.addEventListener("resize", queueReport);
new MutationObserver(queueReport).observe(document.body, { attributes: true, attributeFilter: ["data-panel"] });

requestAnimationFrame(() => invoke("ready"));
