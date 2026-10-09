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

// Enterprise (802.1X) sign-in. Same rule as the password: typed once, sent once, the field emptied at once.
function askEnterprise(line, net) {
  if (asking === net.path) return;
  asking = net.path;
  const form = el("form", "enterprise-form");
  const field = (name, label, type, test) => {
    const wrap = el("label", "field");
    wrap.append(el("span", "", label));
    const input = el("input");
    input.type = type;
    input.autocomplete = "off";
    input.spellcheck = false;
    input.dataset.test = test;
    wrap.append(input);
    form.append(wrap);
    return input;
  };
  const identity = field("identity", "Identity", "text", "ent-identity");
  const password = field("password", "Password", "password", "ent-password");
  const choose = (label, test, options) => {
    const wrap = el("label", "field");
    wrap.append(el("span", "", label));
    const select = el("select");
    select.dataset.test = test;
    options.forEach(([value, text]) => { const option = el("option", "", text); option.value = value; select.append(option); });
    wrap.append(select);
    form.append(wrap);
    return select;
  };
  const eap = choose("Method", "ent-eap", [["peap", "PEAP"], ["ttls", "TTLS"]]);
  const phase2 = choose("Inner authentication", "ent-phase2", [["mschapv2", "MSCHAPv2"], ["pap", "PAP"], ["mschap", "MSCHAP"], ["chap", "CHAP"]]);
  const sync = () => { if (eap.value === "peap") phase2.value = "mschapv2"; [...phase2.options].forEach((o) => { o.disabled = eap.value === "peap" && o.value !== "mschapv2"; }); };
  eap.addEventListener("change", sync);
  sync();
  // The server name is needed to tell the real network from a look-alike, so it is not hidden away.
  const nameLabel = el("label", "field");
  nameLabel.append(el("span", "", "Server name"));
  const domain = el("input");
  domain.type = "text"; domain.autocomplete = "off"; domain.spellcheck = false; domain.placeholder = "radius.example.com";
  domain.dataset.test = "ent-domain";
  nameLabel.append(domain);
  form.append(nameLabel);
  const more = el("details", "more");
  more.append(el("summary", "", "More options"));
  const holder = el("div");
  more.append(holder);
  form.append(more);
  const extra = (label, test) => {
    const wrap = el("label", "field");
    wrap.append(el("span", "", label));
    const input = el("input");
    input.type = "text";
    input.autocomplete = "off";
    input.spellcheck = false;
    input.dataset.test = test;
    wrap.append(input);
    holder.append(wrap);
    return input;
  };
  const anonymous = extra("Anonymous identity", "ent-anonymous");
  const certificate = extra("CA certificate file (full path)", "ent-certificate");
  const buttons = el("div", "pairing-actions");
  const join = el("button", "primary", "Join");
  join.type = "submit";
  join.dataset.test = "ent-join";
  const cancel = el("button", "", "Cancel");
  cancel.type = "button";
  cancel.dataset.test = "ent-cancel";
  const message = el("div", "setting-about warn");
  buttons.append(join, cancel);
  form.append(buttons, message);
  line.after(form);
  identity.focus();
  const close = () => {
    asking = null; password.value = ""; form.remove();
    if (deferred) { const view = deferred; deferred = null; drawNetworkPage(view); }
  };
  cancel.onclick = close;
  form.addEventListener("keydown", (event) => { if (event.key === "Escape") { event.stopPropagation(); close(); } });
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const typed = password.value;
    password.value = "";
    invoke("network_connect_enterprise", { path: net.path, form: { identity: identity.value, anonymous: anonymous.value, password: typed, certificate: certificate.value, domain: domain.value, eap: eap.value, phase2: phase2.value } })
      .then(close, (error) => {
        const text = String(error);
        message.textContent = text.includes("MissingIdentity") ? "Enter your identity."
          : text.includes("MissingServerName") ? "Enter the server name (ask whoever runs the network), or a CA certificate file under More options."
          : text.includes("BadPassword") ? "Enter your password."
          : text.includes("UnsupportedPhase2") ? "PEAP works only with MSCHAPv2."
          : text.includes("BadField") ? "Check the certificate path (it starts with /) and the server name."
          : "That did not work.";
        // A form the person can fix keeps what they typed; the password was never sent.
        if (message.textContent !== "That did not work.") password.value = typed;
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
        : net.action === "enterprise" ? "Enterprise network. Sign in with your account" : net.saved ? "Saved" : net.secured ? "Secured" : "Open";
      const line = listRow(net.ssid, tag, "net:" + net.ssid, { icon: net.secured ? "lock" : "wifi", active: net.active, clickable: joinable || asksPassword || net.action === "enterprise", dim: !joinable && !asksPassword && net.action !== "enterprise" && !net.active });
      if (net.action === "enterprise") line.addEventListener("click", () => askEnterprise(line, net));
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
  const host = $("bt-main");
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
      const pairable = device.action === "pair";
      const battery = device.battery === null || device.battery === undefined ? "" : " · " + device.battery + "% battery";
      const tag = (pairable ? "Not paired"
        : device.action === "busy" ? "Connecting…" : device.connected ? "Connected" : "Paired") + battery;
      const line = listRow(device.name, tag, "bt:" + device.name, { icon: "bluetooth", active: device.connected, clickable: usable || pairable, dim: !usable && !pairable && !device.connected });
      if (device.action === "connect") line.addEventListener("click", () => invoke("bluetooth_connect", { path: device.path }));
      if (device.action === "disconnect") line.addEventListener("click", () => invoke("bluetooth_disconnect", { path: device.path }));
      if (pairable) {
        line.append(actionButton("Pair", "bt-pair:" + device.name, () => invoke("bluetooth_pair", { path: device.path }).catch((error) => console.error(error))));
        line.addEventListener("click", () => invoke("bluetooth_pair", { path: device.path }).catch((error) => console.error(error)));
      }
      if (device.paired) line.append(actionButton("Forget", "bt-forget:" + device.name, () => invoke("bluetooth_forget", { path: device.path })));
      card.append(line);
    });
    if (!view.devices.length) card.append(el("div", "empty", view.scanning ? "Looking for devices…" : "No devices yet. Choose Scan."));
    host.append(card);
  }
  queueReport();
}

// The question a device asks while it is being paired. What is typed goes to Rust once and the field is emptied; nothing is kept.
function drawPairing(view) {
  const host = $("bt-pairing");
  host.replaceChildren();
  if (!view.active) { queueReport(); return; }
  const card = el("div", "rows-card pairing");
  const title = (text) => card.append(el("div", "setting-title", text));
  const row = el("div", "pairing-actions");
  const cancel = actionButton("Cancel", "pair-cancel", () => invoke("bluetooth_pair_cancel"));
  const typed = (label, test, numeric) => {
    const form = el("form", "password-form");
    form.style.padding = "0";
    const field = el("input");
    field.type = "text";
    field.placeholder = label;
    field.autocomplete = "off";
    field.spellcheck = false;
    if (numeric) { field.inputMode = "numeric"; field.maxLength = 6; }
    field.dataset.test = test;
    const send = el("button", "primary", "Pair");
    send.type = "submit";
    send.dataset.test = "pair-send";
    form.append(field, send);
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      const text = field.value;
      field.value = "";
      invoke("bluetooth_pair_answer", { text }).catch(() => { message.textContent = numeric ? "A passkey is up to six digits." : "A PIN is 1 to 16 characters."; field.focus(); });
    });
    card.append(form);
    setTimeout(() => field.focus(), 0);
  };
  const message = el("div", "setting-about warn");
  switch (view.stage) {
    case "pin":
      title("Enter the PIN for " + view.device);
      typed("PIN", "pair-pin", false);
      break;
    case "passkey":
      title("Enter the passkey for " + view.device);
      typed("Passkey", "pair-passkey", true);
      break;
    case "display": {
      title("Type this code on " + view.device);
      const code = el("div", "pair-code");
      code.dataset.test = "pair-code";
      [...view.code].forEach((digit, index) => code.append(el("span", index < view.entered ? "done" : "", digit)));
      card.append(code);
      break;
    }
    case "confirm": {
      title("Does " + view.device + " show this code?");
      const code = el("div", "pair-code", view.code);
      code.dataset.test = "pair-code";
      card.append(code);
      row.append(actionButton("They match", "pair-accept", () => invoke("bluetooth_pair_accept"), "primary"));
      break;
    }
    case "authorize":
      title(view.device + " wants to pair");
      row.append(actionButton("Allow", "pair-accept", () => invoke("bluetooth_pair_accept"), "primary"));
      break;
    default:
      title("Pairing with " + view.device + "…");
  }
  row.append(cancel);
  card.append(message, row);
  host.append(card);
  queueReport();
}

listen("network-view", (event) => drawNetworkPage(event.payload)).then(() => invoke("network_view").then(drawNetworkPage));
listen("bluetooth-view", (event) => drawBluetoothPage(event.payload)).then(() => invoke("bluetooth_view").then(drawBluetoothPage));
listen("pairing-view", (event) => drawPairing(event.payload)).then(() => invoke("pairing_view").then(drawPairing));
