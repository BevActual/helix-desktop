// Mobius hub UI. Rust owns conversations, personas, and routing; this page draws views and sends choices back.
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

let reporting = false;
let reportTimer = null;
let view = { id: "", title: "New chat", messages: [], streaming: false, error: "" };
let status = { inference: "ready", backend: "On this computer", left_device: false };
let personas = { active: "helix", personas: [] };
let stickToBottom = true;

function drawSpans(parent, spans) {
  spans.forEach((span) => {
    const tag = { text: null, code: "code", bold: "strong", italic: "em" }[span.kind];
    if (!tag) parent.append(document.createTextNode(span.text));
    else parent.append(el(tag, "", span.text));
  });
}
function drawBlocks(host, blocks) {
  host.replaceChildren();
  blocks.forEach((block) => {
    if (block.kind === "paragraph") { const p = el("p"); drawSpans(p, block.spans); host.append(p); }
    else if (block.kind === "heading") { const h = el("h" + Math.min(6, Math.max(3, block.level + 2))); drawSpans(h, block.spans); host.append(h); }
    else if (block.kind === "quote") { const q = el("blockquote"); drawSpans(q, block.spans); host.append(q); }
    else if (block.kind === "rule") host.append(el("hr"));
    else if (block.kind === "list") {
      const list = el(block.ordered ? "ol" : "ul");
      block.items.forEach((spans) => { const li = el("li"); drawSpans(li, spans); list.append(li); });
      host.append(list);
    } else if (block.kind === "code") {
      const wrap = el("div", "code-block");
      const bar = el("div", "code-bar");
      bar.append(el("span", "", block.language || "code"));
      const copy = el("button", "mini", "Copy");
      copy.onclick = () => invoke("chat_copy", { text: block.text });
      bar.append(copy);
      const pre = el("pre");
      pre.append(el("code", "", block.text));
      wrap.append(bar, pre);
      host.append(wrap);
    }
  });
}

function typingDots() {
  const dots = el("span", "typing-dots");
  dots.dataset.test = "typing";
  dots.setAttribute("aria-label", "Thinking");
  for (let i = 0; i < 3; i++) dots.append(el("span", "dot"));
  return dots;
}

function messageNode(message, index, last) {
  const row = el("div", "msg " + message.role);
  row.dataset.test = "msg-" + index;
  const body = el("div", "msg-body");
  if (message.role === "user") {
    body.textContent = message.text;
    const actions = el("div", "msg-actions");
    const act = el("button", "mini", "Act on desktop");
    act.dataset.test = "act-" + index;
    act.onclick = () => actOnDesktop(message.text);
    actions.append(act);
    row.append(body, actions);
    return row;
  }
  drawBlocks(body, message.blocks);
  if (!message.blocks.length) body.append(typingDots());
  const actions = el("div", "msg-actions");
  const copy = el("button", "mini", "Copy");
  copy.dataset.test = "copy-" + index;
  copy.onclick = () => invoke("chat_copy", { text: message.text });
  actions.append(copy);
  row.append(body, actions);
  if (last && view.streaming) row.classList.add("live");
  return row;
}

function actOnDesktop(text) {
  invoke("chat_act_on_desktop", { text })
    .then(() => {
      const banner = $("banner");
      banner.textContent = "Sent to the desktop assistant. Confirm there if it asks.";
      banner.hidden = false;
      setTimeout(() => { if (banner.textContent.startsWith("Sent to the desktop")) banner.hidden = true; }, 4000);
    })
    .catch(report);
}

function drawRouteChip() {
  const chip = $("route-chip");
  const label = status.backend || "";
  if (!label) { chip.hidden = true; return; }
  chip.hidden = false;
  chip.textContent = status.left_device ? ("Left device · " + label) : label;
  chip.classList.toggle("remote", !!status.left_device);
}

function drawChat() {
  $("chat-title").textContent = view.title;
  drawRouteChip();
  const host = $("messages");
  host.replaceChildren();
  view.messages.forEach((message, index) => host.append(messageNode(message, index, index === view.messages.length - 1)));
  $("empty").hidden = view.messages.length > 0;
  $("send").hidden = view.streaming;
  $("stop").hidden = !view.streaming;
  $("input").disabled = false;
  const banner = $("banner");
  const text = status.inference !== "ready"
    ? "Inference is not running. It starts when needed; try again in a moment."
    : view.error;
  banner.textContent = text;
  banner.hidden = !text;
  if (stickToBottom) scrollDown();
  queueReport();
}
function scrollDown() {
  const scroller = $("scroller");
  scroller.scrollTop = scroller.scrollHeight;
}
$("scroller").addEventListener("scroll", () => {
  const s = $("scroller");
  stickToBottom = s.scrollHeight - s.scrollTop - s.clientHeight < 60;
});
listen("chat-view", (event) => { view = event.payload; drawChat(); });
listen("chat-stream", (event) => {
  const last = $("messages").lastElementChild;
  if (!last || !last.classList.contains("assistant")) return;
  drawBlocks(last.querySelector(".msg-body"), event.payload.blocks);
  if (stickToBottom) scrollDown();
});

function drawList(items) {
  const host = $("chats");
  host.replaceChildren();
  items.forEach((item, index) => {
    const row = el("div", "chat-row" + (item.id === view.id ? " current" : ""));
    row.dataset.test = "chat-" + index;
    const open = el("button", "chat-open", item.title);
    open.onclick = () => invoke("chat_open", { id: item.id }).catch(report);
    const del = el("button", "chat-delete");
    del.dataset.test = "chat-delete-" + index;
    del.title = "Delete";
    del.setAttribute("aria-label", "Delete chat");
    del.append(icon("trash"));
    del.onclick = (event) => { event.stopPropagation(); invoke("chat_delete", { id: item.id }).catch(report); };
    row.append(open, del);
    host.append(row);
  });
  queueReport();
}
listen("chat-list", (event) => drawList(event.payload));

function drawPersonas(store) {
  personas = store;
  const select = $("persona");
  const previous = select.value;
  select.replaceChildren();
  (store.personas || []).forEach((p) => {
    const opt = document.createElement("option");
    opt.value = p.id;
    opt.textContent = p.name;
    if (p.id === store.active) opt.selected = true;
    select.append(opt);
  });
  if (!select.value && previous) select.value = previous;
}
listen("personas-view", (event) => drawPersonas(event.payload));
$("persona").onchange = () => {
  invoke("personas_set_active", { id: $("persona").value }).catch(report);
};

function report(error) {
  const banner = $("banner");
  banner.textContent = String(error);
  banner.hidden = false;
}

const input = $("input");
function grow() {
  input.style.height = "auto";
  input.style.height = Math.min(180, input.scrollHeight) + "px";
}
input.addEventListener("input", grow);
input.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    $("composer").requestSubmit();
  }
});
$("composer").addEventListener("submit", (event) => {
  event.preventDefault();
  const text = input.value;
  if (!text.trim() || view.streaming) return;
  input.value = "";
  grow();
  stickToBottom = true;
  invoke("chat_send", { text })
    .then(() => invoke("chat_status").then((s) => { status = s; drawRouteChip(); }))
    .catch((error) => { input.value = text; grow(); report(error); });
});
$("stop").onclick = () => invoke("chat_stop");
$("act").onclick = () => {
  const text = input.value.trim() || (view.messages.slice().reverse().find((m) => m.role === "user") || {}).text || "";
  if (!text.trim()) { report("Write something to act on."); return; }
  actOnDesktop(text);
};
$("open-assistant").onclick = () => invoke("chat_open_shell_panel", { id: "assistant" }).catch(report);
$("open-agents").onclick = () => invoke("chat_open_shell_panel", { id: "agents" }).catch(report);
$("open-settings-agents").onclick = () => invoke("open_settings_agents").catch(report);
$("new-chat").onclick = () => invoke("chat_new").then(() => input.focus()).catch(report);

// Routing dialog --------------------------------------------------------------------------------------------------------------------
function attachModelList(ids) {
  const listId = "local-gguf-models";
  let datalist = document.getElementById(listId);
  if (!datalist) {
    datalist = document.createElement("datalist");
    datalist.id = listId;
    document.body.appendChild(datalist);
  }
  datalist.replaceChildren(...ids.map((id) => {
    const option = document.createElement("option");
    option.value = id;
    return option;
  }));
  ["route-chat-model", "route-act-model", "route-code-model"].forEach((inputId) => {
    $(inputId).setAttribute("list", listId);
  });
}

async function openRouting() {
  const routing = await invoke("routing_get");
  $("route-chat-model").value = (routing.profiles && routing.profiles.chat && routing.profiles.chat.model) || "";
  $("route-act-model").value = (routing.profiles && routing.profiles.act && routing.profiles.act.model) || "";
  $("route-code-model").value = (routing.profiles && routing.profiles.code && routing.profiles.code.model) || "";
  $("route-battery").checked = !!routing.allow_remote_on_battery_saver;
  const lan = (routing.backends || []).find((b) => b.id === "lan" || b.kind === "openai");
  $("route-lan-url").value = lan ? (lan.base_url || "") : "";
  const codeBackend = (routing.profiles && routing.profiles.code && routing.profiles.code.backend) || "local";
  $("route-code-backend").value = codeBackend === "local" ? "local" : "lan";
  $("routing-dialog").showModal();
  const models = await invoke("routing_list_models");
  if (Array.isArray(models) && models.length) attachModelList(models);
}
$("open-routing").onclick = () => openRouting().catch(report);
$("routing-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || submitter.value !== "save") return;
  event.preventDefault();
  invoke("routing_get").then((routing) => {
    if (!routing.profiles) routing.profiles = {};
    if (!routing.backends) routing.backends = [];
    const ensureProfile = (name) => {
      if (!routing.profiles[name]) routing.profiles[name] = { backend: "local", model: "" };
    };
    ["act", "chat", "summarize", "vision", "code"].forEach(ensureProfile);
    routing.profiles.chat.model = $("route-chat-model").value.trim();
    routing.profiles.act.model = $("route-act-model").value.trim();
    routing.profiles.act.backend = "local";
    routing.profiles.code.model = $("route-code-model").value.trim();
    routing.allow_remote_on_battery_saver = $("route-battery").checked;
    const url = $("route-lan-url").value.trim();
    let lan = routing.backends.find((b) => b.id === "lan");
    if (url) {
      if (!lan) {
        lan = { id: "lan", kind: "openai", base_url: url, label: "LAN" };
        routing.backends.push(lan);
      } else {
        lan.kind = "openai";
        lan.base_url = url;
        if (!lan.label) lan.label = "LAN";
      }
    }
    routing.profiles.code.backend = $("route-code-backend").value === "lan" && url ? "lan" : "local";
    if (!routing.backends.some((b) => b.id === "local")) {
      routing.backends.unshift({ id: "local", kind: "local", base_url: "", label: "On this computer" });
    }
    return invoke("routing_save", { routing });
  }).then(() => {
    $("routing-dialog").close();
  }).catch(report);
});

(function buildEmpty() {
  const host = $("empty");
  host.append(el("div", "empty-title", "How can I help?"));
  host.append(el("div", "empty-note", "Mobius talks on this computer (or LAN if you grant it). Use Act to change the desktop through the bar Assistant."));
  const suggestions = el("div", "suggestions");
  ["Explain what a cron job is", "Write a bash one-liner to find large files", "Summarise the difference between TCP and UDP", "Help me plan a week of focused work"].forEach((text, index) => {
    const button = el("button", "suggestion", text);
    button.dataset.test = "suggestion-" + index;
    button.onclick = () => { input.value = text; $("composer").requestSubmit(); };
    suggestions.append(button);
  });
  host.append(suggestions);
})();

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

invoke("layout_reporting").then((on) => { reporting = on; if (on) queueReport(); });
invoke("chat_status").then((s) => { status = s; drawChat(); });
invoke("personas_get").then(drawPersonas);
invoke("chat_view").then((v) => { view = v; drawChat(); invoke("chat_list").then(drawList); });
input.focus();
