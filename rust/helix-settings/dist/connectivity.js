// Network and Bluetooth pages. Rust holds the state and builds every request (the same models as the control center); this draws the lists
// it was sent and names a network or device only by the path it was listed with. Text goes in through textContent only.
function switchRow(title, about, on, test, disabled, onChange) {
  const line = el("div", "setting-row");
  const text = el("div", "setting-text");
  text.append(el("div", "setting-title", title));
  if (about) text.append(el("div", "setting-about", about));
  const box = el("input");
  box.type = "checkbox";
  box.className = "switch";
  box.checked = on;
  box.disabled = !!disabled;
  box.dataset.test = test;
  box.addEventListener("change", () => onChange(box.checked));
  line.append(text, box);
  return line;
}

function listRow(title, tag, test, state) {
  const line = el("div", "setting-row list-row" + (state.active ? " active" : "") + (state.clickable ? " clickable" : "") + (state.dim ? " dim" : ""));
  line.dataset.test = test;
  const lead = el("div", "row-lead");
  if (state.icon) lead.append(icon(state.icon));
  const text = el("div", "setting-text");
  text.append(el("div", "setting-title", title));
  if (tag) text.append(el("div", "setting-about", tag));
  lead.append(text);
  line.append(lead);
  return line;
}

function actionButton(label, test, onClick, kind) {
  const button = el("button", kind || "", label);
  button.dataset.test = test;
  button.addEventListener("click", (event) => { event.stopPropagation(); onClick(); });
  return button;
}

// The password is typed into a field that is removed as soon as it is sent; it is never stored or drawn back.
let asking = null;
let deferred = null;   // a newer view that arrived while a password was being typed
function askPassword(line, net) {
  if (asking === net.path) return;
  asking = net.path;
  const form = el("form", "password-form");
  const field = el("input");
  field.type = "password";
  field.placeholder = "Password";
  field.autocomplete = "off";
  field.spellcheck = false;
  field.dataset.test = "wifi-password";
  const join = el("button", "primary", "Join");
  join.type = "submit";
  join.dataset.test = "wifi-join";
  const cancel = el("button", "", "Cancel");
  cancel.type = "button";
  cancel.dataset.test = "wifi-cancel";
  const message = el("div", "setting-about warn");
  form.append(field, join, cancel);
  line.after(form);
  form.after(message);
  field.focus();
  const close = () => {
    asking = null; field.value = ""; form.remove(); message.remove();
    if (deferred) { const view = deferred; deferred = null; drawNetworkPage(view); }
  };
  cancel.onclick = close;
  form.addEventListener("keydown", (event) => { if (event.key === "Escape") { event.stopPropagation(); close(); } });
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const password = field.value;
    field.value = "";
    invoke("network_connect_password", { path: net.path, password }).then(close, (error) => {
      message.textContent = String(error).includes("BadPassword") ? "A Wi-Fi password has 8 to 63 characters." : "That did not work.";
      field.focus();
    });
  });
}

function drawNetworkPage(view) {
  if (asking) { deferred = view; return; }
  const host = $("network-host");
  host.replaceChildren();
  if (!view.available) { host.append(el("div", "empty", "Networking is not available on this computer.")); queueReport(); return; }
  const top = el("div", "rows-card");
  top.append(switchRow("Wi-Fi", view.status, view.wifi_enabled, "wifi-switch", false, (on) => invoke("network_set_wifi", { enabled: on })));
  host.append(top);
  if (view.wifi_enabled) {
    const heading = el("div", "section-head");
    heading.append(el("h4", "", "Networks"));
    const tools = el("div", "section-tools");
    if (view.connected) tools.append(actionButton("Disconnect", "wifi-disconnect", () => invoke("network_disconnect")));
    const scan = actionButton(view.scanning ? "Scanning…" : "Scan", "wifi-scan", () => invoke("network_scan"));
    scan.disabled = view.scanning;
    tools.append(scan);
    heading.append(tools);
    host.append(heading);
    const card = el("div", "rows-card");
    view.networks.forEach((net) => {
      const joinable = net.action === "connect";
      const asksPassword = net.action === "password";
      const tag = net.active ? "Connected"
        : asksPassword ? "Password required"
        : net.action === "enterprise" ? "Enterprise network" : net.saved ? "Saved" : net.secured ? "Secured" : "Open";
      const line = listRow(net.ssid, tag, "net:" + net.ssid, { icon: net.secured ? "lock" : "wifi", active: net.active, clickable: joinable || asksPassword, dim: !joinable && !asksPassword && !net.active });
      if (asksPassword) line.addEventListener("click", () => askPassword(line, net));
      if (joinable) line.addEventListener("click", () => invoke("network_connect", { path: net.path }));
      if (net.saved) line.append(actionButton("Forget", "net-forget:" + net.ssid, () => invoke("network_forget", { path: net.path })));
      card.append(line);
    });
    if (!view.networks.length) card.append(el("div", "empty", "No networks found."));
    host.append(card);
  }
  if (view.vpns.length) {
    host.append(el("h4", "", "VPN"));
    const card = el("div", "rows-card");
    view.vpns.forEach((vpn) => card.append(switchRow(vpn.name, "", vpn.active, "vpn:" + vpn.name, false, (on) => invoke("network_vpn", { path: vpn.path, active: on }))));
    host.append(card);
  }
  queueReport();
}

function drawBluetoothPage(view) {
  const host = $("bluetooth-host");
  host.replaceChildren();
  if (!view.available) { host.append(el("div", "empty", "Bluetooth is not available on this computer.")); queueReport(); return; }
  const top = el("div", "rows-card");
  top.append(switchRow("Bluetooth", view.blocked ? "Blocked by the system" : view.status, view.enabled, "bt-switch", view.blocked, (on) => invoke("bluetooth_set_power", { enabled: on })));
  host.append(top);
  if (view.enabled) {
    const heading = el("div", "section-head");
    heading.append(el("h4", "", "Devices"));
    const tools = el("div", "section-tools");
    tools.append(actionButton(view.scanning ? "Stop" : "Scan", "bt-scan", () => invoke("bluetooth_scan", { on: !view.scanning })));
    heading.append(tools);
    host.append(heading);
    const card = el("div", "rows-card");
    view.devices.forEach((device) => {
      const usable = device.action === "connect" || device.action === "disconnect";
      const battery = device.battery === null || device.battery === undefined ? "" : " · " + device.battery + "% battery";
      const tag = (device.action === "pair" ? "Not paired. Pairing is not available here yet"
        : device.action === "busy" ? "Connecting…" : device.connected ? "Connected" : "Paired") + battery;
      const line = listRow(device.name, tag, "bt:" + device.name, { icon: "bluetooth", active: device.connected, clickable: usable, dim: !usable && !device.connected });
      if (device.action === "connect") line.addEventListener("click", () => invoke("bluetooth_connect", { path: device.path }));
      if (device.action === "disconnect") line.addEventListener("click", () => invoke("bluetooth_disconnect", { path: device.path }));
      if (device.paired) line.append(actionButton("Forget", "bt-forget:" + device.name, () => invoke("bluetooth_forget", { path: device.path })));
      card.append(line);
    });
    if (!view.devices.length) card.append(el("div", "empty", view.scanning ? "Looking for devices…" : "No devices yet. Choose Scan."));
    host.append(card);
  }
  queueReport();
}

listen("network-view", (event) => drawNetworkPage(event.payload)).then(() => invoke("network_view").then(drawNetworkPage));
listen("bluetooth-view", (event) => drawBluetoothPage(event.payload)).then(() => invoke("bluetooth_view").then(drawBluetoothPage));
