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
let view = { id: "", title: "New chat", messages: [], streaming: false, error: "", temporary: false, pinned: false, project: "" };
let status = { inference: "ready", backend: "On this computer", left_device: false };
let personas = { active: "helix", personas: [] };
let lab = null; // the experiments and this chat's style, from the React dialogs
let editingPersonaId = "helix";
let editingMessageIndex = null;
let stickToBottom = true;
let chatItems = [];
let chatFilter = "";
let projectFilter = "";
let currentModel = "";
let currentBackend = "local";
let searchHits = null;
let searchTimer = null;
// Follow-ups typed while a reply is being written, sent one after another when it ends (at most MAX_QUEUE).
const MAX_QUEUE = 5;
let queue = [];
// Friendly names of the installed models (file name -> "Qwen2.5 VL 7B"), for the chip beside the composer.
let modelTitles = {};
function refreshModelTitles() {
  return invoke("library_list")
    .then((cards) => { modelTitles = {}; (cards || []).filter((c) => c.installed).forEach((c) => { modelTitles[c.file] = c.title; }); drawComposerMeta(); })
    .catch(() => undefined);
}
let promptHistory = [];
let promptHistoryIndex = -1;
let promptDraft = "";
let slashIndex = 0;
let pendingAttachments = [];
let attachmentWarnings = [];
let attachmentMaxChars = 8000;
let attachmentMaxFiles = 6;
let pickingAttachments = false;
let agentDeskGlance = null;
let pendingPictures = []; // { id, url } staged in Rust, shown in the strip above the composer
let findMatches = [];
let findIndex = -1;

const PROMPT_HISTORY_KEY = "mobius.promptHistory";
const SLASH_COMMANDS = [
  { cmd: "/image", desc: "Generate an image on this computer", run: (typed) => {
    const prompt = (typed || "").replace(/^\/image\b/i, "").trim();
    if (!prompt) { openCreateImage(); return Promise.resolve(); }
    return invoke("chat_generate_image", { prompt, model: null, width: null, height: null, seed: null });
  } },
  { cmd: "/new", desc: "Start a new chat", run: () => invoke("chat_new").then(() => { input.focus(); closeSideDrawer(); }) },
  { cmd: "/temp", desc: "Temporary chat (not saved)", run: () => invoke("chat_temporary").then(() => { input.focus(); closeSideDrawer(); }) },
  { cmd: "/model", desc: "Pick chat model / backend", run: () => openModelPicker() },
  { cmd: "/models", desc: "Models & routing page", run: () => openRouting() },
  { cmd: "/route", desc: "Models & routing page", run: () => openRouting() },
  { cmd: "/attach", desc: "Attach text / source files", run: () => pickAttachments() },
  { cmd: "/web", desc: "Pick online sources yourself", run: (text) => { openWeb(text.replace(/^\/web\s*/i, "")); return Promise.resolve(); } },
  { cmd: "/tools", desc: "Configure Tools mode (research + project commands)", run: () => { reactTools?.open(view); return Promise.resolve(); } },
  { cmd: "/desk", desc: "Show the Helix agent desk workspace", run: () => invoke("agent_desk_focus").then(refreshAgentDeskGlance).catch(report) },
  { cmd: "/context", desc: "Review recent history and chat brief", run: () => { openChatContext(); return Promise.resolve(); } },
  { cmd: "/find", desc: "Find in this chat", run: () => { openFindBar(); return Promise.resolve(); } },
  { cmd: "/fork", desc: "Fork this chat", run: () => forkChat() },
  { cmd: "/export", desc: "Copy chat as Markdown", run: () => $("export-chat").click() },
  { cmd: "/retry", desc: "Regenerate the last reply", run: () => invoke("chat_regenerate").catch(report) },
  { cmd: "/stop", desc: "Stop the current reply", run: () => invoke("chat_stop") },
  { cmd: "/act", desc: "Have Mobius act on the desktop", run: (typed) => {
    const rest = (typed || "").replace(/^\/act\b/i, "").trim();
    const text = rest || lastActableText();
    if (!text.trim()) { report("Write something to act on."); return Promise.resolve(); }
    return actOnDesktop(text);
  } },
  { cmd: "/search", desc: "Focus chat search", run: () => {
    openSideDrawer();
    $("chat-search").focus();
    $("chat-search").select();
    return Promise.resolve();
  } },
  { cmd: "/note", desc: "Write down something Mobius should do better", run: (typed) => {
    const text = (typed || "").replace(/^\/note\b/i, "").trim();
    if (!text) { report("Write what you wish it did: /note <text>"); return Promise.resolve(); }
    return invoke("lab_note", { text }).then((path) => {
      info("Noted. Saved to " + path);
    });
  } },
  { cmd: "/usage", desc: "How Mobius has been used (last 7 days)", run: () => invoke("lab_usage", { days: 7 }).then((text) => info(text, 12000)) },
  { cmd: "/style", desc: "Set this chat's style (concise, teacher, …)", run: () => { openMeta("style"); return Promise.resolve(); } },
  { cmd: "/persona", desc: "Switch persona", run: () => { openMeta("persona"); return Promise.resolve(); } },
  { cmd: "/help", desc: "Show slash commands", run: () => { showSlashMenu(""); return Promise.resolve(); } },
];

function lastActableText() {
  const msgs = view.messages || [];
  for (let i = msgs.length - 1; i >= 0; i--) {
    const m = msgs[i];
    if (m && m.text && m.text.trim()) return m.text.trim();
  }
  return "";
}

// The attachment chips and the slash menu are drawn by React when the bundle is present; the state and logic stay here.
const reactChips = window.HelixMobiusReact
  ? window.HelixMobiusReact.chips($("attach-chips"), (index) => { pendingAttachments.splice(index, 1); drawAttachChips(); }, () => { attachmentWarnings = []; drawAttachChips(); })
  : null;
let slashShown = [];
const reactSlash = window.HelixMobiusReact
  ? window.HelixMobiusReact.slash($("slash-menu"), (index) => { if (slashShown[index]) runSlash(slashShown[index]); })
  : null;

function drawAttachChips() {
  const host = $("attach-chips");
  if (reactChips) {
    host.hidden = !pendingAttachments.length && !attachmentWarnings.length;
    let budget = attachmentMaxChars;
    const files = pendingAttachments.map((file) => {
      const included_chars = Math.min(file.chars, budget);
      budget -= included_chars;
      return { ...file, included_chars };
    });
    reactChips.set(files, attachmentWarnings);
    syncSendEnabled();
    return;
  }
  if (!pendingAttachments.length && !attachmentWarnings.length) {
    host.hidden = true;
    host.replaceChildren();
    syncSendEnabled();
    return;
  }
  host.hidden = false;
  host.replaceChildren();
  let budget = attachmentMaxChars;
  pendingAttachments.forEach((file, index) => {
    const included = Math.min(file.chars, budget);
    budget -= included;
    const chip = el("div", "attach-chip");
    chip.append(el("span", "", file.name + " · " + included + " of " + file.original_chars + " characters"));
    const remove = el("button", "", "×");
    remove.type = "button";
    remove.title = "Remove";
    remove.onclick = () => {
      pendingAttachments.splice(index, 1);
      drawAttachChips();
    };
    chip.append(remove);
    host.append(chip);
  });
  attachmentWarnings.forEach((notice) => host.append(el("p", "attachment-note", notice)));
  syncSendEnabled();
}

function pickAttachments() {
  if (pickingAttachments) return Promise.resolve();
  pickingAttachments = true;
  $("attach").disabled = true;
  $("attach").setAttribute("aria-busy", "true");
  return invoke("chat_pick_attachments")
    .then((selection) => {
      const list = Array.isArray(selection.files) ? selection.files : [];
      attachmentWarnings = selection.warnings || [];
      attachmentMaxChars = selection.max_chars;
      attachmentMaxFiles = selection.max_files;
      let skipped = 0;
      list.forEach((file) => {
        if (pendingAttachments.some((a) => a.path === file.path)) return;
        if (pendingAttachments.length >= attachmentMaxFiles) { skipped++; return; }
        pendingAttachments.push(file);
      });
      if (skipped) attachmentWarnings.push(`${skipped} selected file${skipped === 1 ? " was" : "s were"} not added. A message can include up to ${attachmentMaxFiles} files.`);
      drawAttachChips();
      input.focus();
    })
    .catch(report)
    .finally(() => {
      pickingAttachments = false;
      $("attach").disabled = false;
      $("attach").removeAttribute("aria-busy");
    });
}

// Pictures --------------------------------------------------------------------------------------------------------------------------
const pictureUrls = new Map();
function pictureUrl(id) {
  if (!pictureUrls.has(id)) {
    pictureUrls.set(id, invoke("image_data", { id }).then((bytes) => URL.createObjectURL(new Blob([bytes]))));
  }
  return pictureUrls.get(id);
}

function drawPictureStrip() {
  const host = $("picture-strip");
  host.hidden = !pendingPictures.length;
  host.replaceChildren();
  pendingPictures.forEach((picture, index) => {
    const tile = el("div", "picture-tile");
    tile.dataset.test = "picture-tile-" + index;
    const img = el("img");
    img.alt = "Picture to send";
    img.src = picture.url;
    const remove = el("button", "", "×");
    remove.type = "button";
    remove.title = "Remove";
    remove.dataset.test = "picture-remove-" + index;
    remove.onclick = () => {
      invoke("image_discard", { id: picture.id }).catch(() => {});
      pendingPictures.splice(index, 1);
      drawPictureStrip();
    };
    tile.append(img, remove);
    host.append(tile);
  });
  syncSendEnabled();
}

function addPictures(ids) {
  for (const id of ids) {
    if (pendingPictures.length >= 4) { report("A message can carry up to 4 pictures."); invoke("image_discard", { id }).catch(() => {}); continue; }
    pictureUrl(id).then((url) => {
      pendingPictures.push({ id, url });
      drawPictureStrip();
    }).catch(report);
  }
  input.focus();
}

function addPictureFiles(files) {
  const pictures = Array.from(files).filter((file) => file.type.startsWith("image/"));
  pictures.forEach((file) => {
    if (file.size > 8 * 1024 * 1024) { report("Pictures can be up to 8 MB."); return; }
    file.arrayBuffer()
      .then((bytes) => invoke("image_stage", new Uint8Array(bytes)))
      .then((id) => addPictures([id]))
      .catch(report);
  });
  return pictures.length > 0;
}

function pickPictures() {
  return invoke("image_pick").then((ids) => { if (ids.length) addPictures(ids); }).catch(report);
}

// Dictation -------------------------------------------------------------------------------------------------------------------------
// The microphone is open only between the first click and the second (or Escape). Rust records and transcribes locally; the words
// are put into the composer, never sent on their own.
let listening = false;
let dictationBusy = "";
let listenTimer = null;
let listenStarted = 0;
const dictateButton = $("dictate");

function drawListening(seconds) {
  const m = Math.floor(seconds / 60);
  const s = String(Math.floor(seconds % 60)).padStart(2, "0");
  input.placeholder = "Listening… " + m + ":" + s + " — click the microphone to finish, Esc to cancel";
}

function setListening(on) {
  listening = on;
  drawSuggestions();
  dictateButton.classList.toggle("recording", on);
  dictateButton.setAttribute("aria-pressed", on ? "true" : "false");
  dictateButton.title = on ? "Finish dictation" : "Dictate";
  clearInterval(listenTimer);
  if (on) {
    listenStarted = Date.now();
    drawListening(0);
    listenTimer = setInterval(() => drawListening((Date.now() - listenStarted) / 1000), 500);
  } else {
    composerPlaceholder();
    dictateButton.style.removeProperty("--level");
  }
}

function insertAtCursor(text) {
  const start = input.selectionStart == null ? input.value.length : input.selectionStart;
  const end = input.selectionEnd == null ? start : input.selectionEnd;
  const before = input.value.slice(0, start);
  const after = input.value.slice(end);
  const lead = before && !/\s$/.test(before) ? " " : "";
  input.value = before + lead + text + after;
  const caret = (before + lead + text).length;
  input.setSelectionRange(caret, caret);
  grow();
  syncSendEnabled();
}

dictateButton.onclick = () => {
  if (dictateButton.disabled) return;
  if (!listening) {
    dictateButton.disabled = true;
    dictationBusy = "starting";
    drawSuggestions();
    invoke("dictate_start").then(() => setListening(true)).catch(report).finally(() => {
      dictateButton.disabled = false;
      dictationBusy = "";
      drawSuggestions();
    });
    return;
  }
  setListening(false);
  dictateButton.disabled = true;
  dictationBusy = "transcribing";
  drawSuggestions();
  input.placeholder = "Transcribing…";
  invoke("dictate_stop")
    .then((text) => {
      if (text) insertAtCursor(text);
      else report("Nothing was heard.");
    })
    .catch(report)
    .finally(() => {
      dictateButton.disabled = false;
      dictationBusy = "";
      drawSuggestions();
      composerPlaceholder();
      input.focus();
    });
};
listen("dictate-level", (event) => {
  if (listening) dictateButton.style.setProperty("--level", String(Math.min(1, event.payload * 3)));
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && listening) {
    event.preventDefault();
    invoke("dictate_cancel").catch(() => {});
    setListening(false);
  }
}, true);

function clearPictures() {
  pendingPictures = [];
  drawPictureStrip();
}

function clearAttachments() {
  pendingAttachments = [];
  attachmentWarnings = [];
  drawAttachChips();
}

function attachmentPayload() {
  return pendingAttachments.map((a) => ({ name: a.name, content: a.content, original_chars: a.original_chars }));
}

function openFindBar() {
  $("find-bar").hidden = false;
  $("find-input").focus();
  $("find-input").select();
  runFind();
}

function closeFindBar() {
  if (reactThread) reactThread.find("");
  $("find-bar").hidden = true;
  $("find-input").value = "";
  findMatches = [];
  findIndex = -1;
  clearFindHighlights();
  $("find-count").textContent = "";
}

function clearFindHighlights() {
  if (reactThread) return; // the thread draws its own highlights from the needle
  document.querySelectorAll("mark.find-hit").forEach((mark) => {
    const parent = mark.parentNode;
    if (!parent) return;
    parent.replaceChild(document.createTextNode(mark.textContent), mark);
    parent.normalize();
  });
}

function highlightInNode(node, needle) {
  if (!needle || node.nodeType !== Node.ELEMENT_NODE) return;
  if (node.matches && node.matches("mark.find-hit")) return;
  const walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT);
  const texts = [];
  while (walker.nextNode()) texts.push(walker.currentNode);
  const lowerNeedle = needle.toLowerCase();
  texts.forEach((textNode) => {
    const value = textNode.nodeValue || "";
    const lower = value.toLowerCase();
    let start = 0;
    let idx = lower.indexOf(lowerNeedle, start);
    if (idx === -1) return;
    const frag = document.createDocumentFragment();
    while (idx !== -1) {
      if (idx > start) frag.append(document.createTextNode(value.slice(start, idx)));
      const mark = document.createElement("mark");
      mark.className = "find-hit";
      mark.textContent = value.slice(idx, idx + needle.length);
      frag.append(mark);
      findMatches.push(mark);
      start = idx + needle.length;
      idx = lower.indexOf(lowerNeedle, start);
    }
    if (start < value.length) frag.append(document.createTextNode(value.slice(start)));
    textNode.parentNode.replaceChild(frag, textNode);
  });
}

function runFind() {
  if (reactThread) {
    const needle = $("find-input").value.trim();
    reactThread.find(needle);
    findMatches = Array.from(document.querySelectorAll("#messages mark.find-hit"));
    findIndex = -1;
    if (!needle) { $("find-count").textContent = ""; return; }
    $("find-count").textContent = findMatches.length ? ("1 / " + findMatches.length) : "0";
    if (findMatches.length) { findIndex = 0; focusFindMatch(0); }
    return;
  }
  clearFindHighlights();
  findMatches = [];
  findIndex = -1;
  const needle = $("find-input").value.trim();
  if (!needle) {
    $("find-count").textContent = "";
    return;
  }
  document.querySelectorAll("#messages .msg-body").forEach((body) => highlightInNode(body, needle));
  $("find-count").textContent = findMatches.length ? ("1 / " + findMatches.length) : "0";
  if (findMatches.length) {
    findIndex = 0;
    focusFindMatch(0);
  }
}

function focusFindMatch(index) {
  if (!findMatches.length) return;
  findIndex = (index + findMatches.length) % findMatches.length;
  findMatches.forEach((m, i) => m.classList.toggle("current", i === findIndex));
  $("find-count").textContent = (findIndex + 1) + " / " + findMatches.length;
  findMatches[findIndex].scrollIntoView({ block: "center", behavior: "smooth" });
}

try {
  const raw = localStorage.getItem(PROMPT_HISTORY_KEY);
  const parsed = raw ? JSON.parse(raw) : [];
  if (Array.isArray(parsed)) promptHistory = parsed.filter((t) => typeof t === "string" && t.trim()).slice(0, 50);
} catch (_) { /* ignore */ }

function rememberPrompt(text) {
  const trimmed = text.trim();
  if (!trimmed || trimmed.startsWith("/")) return;
  promptHistory = [trimmed, ...promptHistory.filter((t) => t !== trimmed)].slice(0, 50);
  try { localStorage.setItem(PROMPT_HISTORY_KEY, JSON.stringify(promptHistory)); } catch (_) { /* ignore */ }
  promptHistoryIndex = -1;
  promptDraft = "";
}

function forkChat(throughIndex) {
  closeHubMenu();
  return invoke("chat_fork", { throughIndex: throughIndex == null ? null : throughIndex })
    .then(() => { input.focus(); closeSideDrawer(); })
    .catch(report);
}

function drawQueue() {
  const list = $("queue-list");
  list.replaceChildren();
  list.hidden = queue.length === 0;
  queue.forEach((text, index) => {
    const row = el("div", "queue-item");
    row.dataset.test = `queue-item-${index}`;
    const label = el("span", "queue-text", text.length > 80 ? text.slice(0, 80) + "…" : text);
    label.title = text;
    const now = el("button", "mini", "Send now");
    now.type = "button";
    now.dataset.test = `queue-now-${index}`;
    now.title = "Stop the reply being written and send this next";
    now.onclick = () => {
      queue.unshift(queue.splice(index, 1)[0]);
      drawQueue();
      if (view.streaming) invoke("chat_stop"); else flushQueue();
    };
    const drop = el("button", "mini", "Remove");
    drop.type = "button";
    drop.dataset.test = `queue-remove-${index}`;
    drop.setAttribute("aria-label", "Remove this queued message");
    drop.onclick = () => { queue.splice(index, 1); drawQueue(); input.focus(); };
    row.append(el("span", "queue-mark", "Next"), label, now, drop);
    list.append(row);
  });
}

function flushQueue() {
  if (!queue.length || view.streaming) return;
  const text = queue.shift();
  drawQueue();
  stickToBottom = true;
  invoke("chat_send", { text, attachments: null })
    .then(() => invoke("chat_status").then((s) => { status = s; drawComposerMeta(); }))
    .catch((error) => { queue.unshift(text); drawQueue(); report(error); });
}

function updateJumpLatest() {
  $("jump-latest").hidden = stickToBottom || view.messages.length === 0;
}

function filteredSlash(filter) {
  const needle = (filter || "").toLowerCase();
  return SLASH_COMMANDS.filter((c) => !needle || c.cmd.slice(1).startsWith(needle) || c.cmd.includes(needle) || c.desc.toLowerCase().includes(needle));
}

function hideSlashMenu() {
  $("slash-menu").hidden = true;
  slashShown = [];
  if (reactSlash) reactSlash.set([], 0);
  else $("slash-menu").replaceChildren();
}

function showSlashMenu(filter) {
  const host = $("slash-menu");
  const items = filteredSlash(filter);
  if (!items.length) { hideSlashMenu(); return; }
  slashIndex = Math.max(0, Math.min(slashIndex, items.length - 1));
  if (reactSlash) {
    slashShown = items;
    reactSlash.set(items.map((item) => ({ cmd: item.cmd, desc: item.desc })), slashIndex);
    host.hidden = false;
    return;
  }
  host.replaceChildren();
  items.forEach((item, index) => {
    const row = el("button", "slash-item" + (index === slashIndex ? " active" : ""));
    row.type = "button";
    row.append(el("span", "slash-cmd", item.cmd), el("span", "slash-desc", item.desc));
    row.onmousedown = (event) => { event.preventDefault(); runSlash(item); };
    host.append(row);
  });
  host.hidden = false;
}

function runSlash(item) {
  hideSlashMenu();
  const typed = input.value; // commands that take text (/note, /act) read it here; the field is cleared first
  input.value = "";
  grow();
  Promise.resolve(item.run(typed)).catch(report);
}

function maybeSlashFromInput() {
  const value = input.value;
  if (!value.startsWith("/")) { hideSlashMenu(); return false; }
  if (value.includes("\n")) { hideSlashMenu(); return false; }
  const space = value.indexOf(" ");
  if (space !== -1 && !value.startsWith("/web") && !value.startsWith("/image") && !value.startsWith("/act") && !value.startsWith("/search") && !value.startsWith("/help")) {
    hideSlashMenu();
    return false;
  }
  const filter = space === -1 ? value.slice(1) : value.slice(1, space);
  showSlashMenu(filter);
  return true;
}

function tryRunSlashSubmit() {
  const value = input.value.trim();
  if (!value.startsWith("/")) return false;
  const token = value.split(/\s+/)[0].toLowerCase();
  const item = SLASH_COMMANDS.find((c) => c.cmd === token);
  if (!item) {
    const open = filteredSlash(token.slice(1));
    if (open.length === 1) {
      runSlash(open[0]);
      return true;
    }
    showSlashMenu(token.slice(1));
    return true;
  }
  runSlash(item);
  return true;
}

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
    if (message.images && message.images.length) {
      const strip = el("div", "msg-pictures");
      message.images.forEach((id) => {
        const img = el("img", "msg-picture");
        img.alt = "Attached picture";
        img.dataset.test = "msg-picture";
        pictureUrl(id).then((url) => { img.src = url; }).catch(() => {});
        strip.append(img);
      });
      row.append(strip);
    }
    const actions = el("div", "msg-actions");
    const edit = el("button", "mini", "Edit");
    edit.dataset.test = "edit-" + index;
    edit.onclick = () => editUserMessage(index, message.text);
    const act = el("button", "mini", "Act");
    act.dataset.test = "act-" + index;
    act.title = "Have Mobius change the desktop using this";
    act.onclick = () => actOnDesktop(message.text);
    const quote = el("button", "mini", "Quote");
    quote.dataset.test = "quote-" + index;
    quote.title = "Insert into the composer";
    quote.onclick = () => {
      const prefix = input.value && !input.value.endsWith("\n") ? "\n\n" : "";
      input.value = input.value + prefix + message.text;
      grow();
      syncSendEnabled();
      input.focus();
    };
    actions.append(edit, act, quote);
    if (!view.streaming) {
      const fork = el("button", "mini", "Fork");
      fork.dataset.test = "fork-user-" + index;
      fork.title = "Continue from here in a new chat";
      fork.onclick = () => forkChat(index);
      actions.append(fork);
    }
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
  if (!view.streaming && message.text.trim()) {
    const act = el("button", "mini", "Act");
    act.dataset.test = "act-assistant-" + index;
    act.title = "Have Mobius act from this reply";
    act.onclick = () => actOnDesktop(message.text);
    actions.append(act);
  }
  if (!view.streaming) {
    const fork = el("button", "mini", "Fork");
    fork.dataset.test = "fork-" + index;
    fork.title = "Continue from here in a new chat";
    fork.onclick = () => forkChat(index);
    actions.append(fork);
  }
  if (last && !view.streaming) {
    const regen = el("button", "mini", "Regenerate");
    regen.dataset.test = "regenerate";
    regen.onclick = () => invoke("chat_regenerate").catch(report);
    actions.append(regen);
  }
  row.append(body, actions);
  if (last && view.streaming) row.classList.add("live");
  return row;
}

function editUserMessage(index, current) {
  if (reactDialogs) { reactDialogs.openEdit(index, current); return; }
  editingMessageIndex = index;
  $("edit-text").value = current;
  $("edit-dialog").showModal();
  $("edit-text").focus();
}

$("edit-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || submitter.value !== "save") return;
  event.preventDefault();
  const text = $("edit-text").value.trim();
  if (!text) { report("Write something to send."); return; }
  const index = editingMessageIndex;
  editingMessageIndex = null;
  $("edit-dialog").close();
  stickToBottom = true;
  invoke("chat_edit", { index, text })
    .then(() => invoke("chat_status").then((s) => { status = s; drawComposerMeta(); }))
    .catch(report);
});

// The task the desktop assistant is doing for a request handed over from this window: working, waiting for approval, done, or why not.
// Approval itself is only ever given in the shell; the card can bring that back (Review) or cancel (Stop).
let dismissedTask = 0;
const reactAct = window.HelixMobiusReact?.acttask
  ? window.HelixMobiusReact.acttask($("act-task-host"), {
      stop: () => invoke("chat_act_stop").catch(report),
      review: () => invoke("chat_open_shell_panel", { id: "assistant" }).catch(report),
      dismiss: () => { dismissedTask = lastActTask; reactAct.set(null); },
    })
  : null;
let lastActTask = 0;
function showAct(state) {
  if (!reactAct || !state || !state.mine) return;
  // Stopped (or finished and cleared) in the shell: the card goes with it.
  if (state.phase === "idle") { reactAct.set(null); return; }
  lastActTask = state.task;
  if (state.task <= dismissedTask) return;
  reactAct.set(state);
}
if (reactAct) {
  listen("act-state", (event) => showAct(event.payload));
}
function actOnDesktop(text, handedOver) {
  return invoke("chat_act_on_desktop", { text })
    .then(() => {
      if (handedOver) handedOver();
      if (reactAct) return;   // the card above the composer takes it from here
      const banner = $("banner");
      banner.textContent = "Sent to the desktop assistant. Approve it in the bar if it asks.";
      banner.hidden = false;
      setTimeout(() => { if (banner.textContent.startsWith("Sent to the desktop assistant")) banner.hidden = true; }, 4000);
    })
    .catch(report);
}

function activePersonaNow() {
  const list = personas.personas || [];
  return list.find((p) => p.id === personas.active) || list[0] || null;
}

function useStarter(text) {
  if (input.value.trim()) { input.focus(); report("Your draft is still here. Clear it to use a starter."); return; }
  input.value = text;
  grow(); syncSendEnabled(); input.focus();
}

const DEFAULT_STARTERS = [
  "Help me plan a focused week",
  "Draft a polite status update",
  "Explain a technical concept step by step",
];
let suggestionKey = "";
let reactWelcome = null;
// An empty chat offers the persona's own starters when that experiment is on, the generic ones otherwise.
function drawSuggestions() {
  const host = document.querySelector("#empty .suggestions");
  if (!host && !reactWelcome) return;
  const persona = activePersonaNow();
  const own = lab && lab.experiments.persona_starters && persona && persona.starters && persona.starters.length ? persona.starters : null;
  const list = own || DEFAULT_STARTERS;
  const key = list.join("\n") + "|" + listening + "|" + dictationBusy;
  if (key === suggestionKey) return;
  suggestionKey = key;
  if (reactWelcome) { reactWelcome.set(list, listening, dictationBusy); return; }
  host.replaceChildren();
  list.forEach((text, index) => {
    const button = el("button", "suggestion", text);
    button.dataset.test = "suggestion-" + index;
    button.onclick = () => useStarter(text);
    host.append(button);
  });
}

function openMeta(kind) {
  const anchor = $(kind === "persona" ? "meta-persona" : "meta-style");
  const box = (!anchor.hidden ? anchor : $("composer")).getBoundingClientRect();
  if (reactDialogs) reactDialogs.openMenu(kind, { left: box.left, top: box.top, width: box.width });
}

function drawLabChips() {
  const chipOn = !!(lab && lab.experiments.persona_chip);
  const persona = activePersonaNow();
  $("meta-persona").hidden = !chipOn;
  $("meta-persona").textContent = persona ? persona.name : "Helix";
  $("meta-persona").classList.toggle("remote", false);
  // The rail under the title is the way to switch persona only while the chip experiment is off.
  const rail = $("persona-rail");
  if (rail) rail.hidden = chipOn;
  const styleOn = !!(lab && lab.experiments.style_overlays);
  $("meta-style").hidden = !styleOn;
  const current = styleOn && lab.style ? (lab.styles.find((s) => s.id === lab.style) || { label: lab.style }).label : "";
  $("meta-style").textContent = current ? "Style · " + current : "Style";
  drawSuggestions();
}

function toolsChipLabel() {
  if (!view.tools_enabled) return "Tools · off";
  if (!view.workspace) return "Tools · on";
  const parts = view.workspace.split("/").filter(Boolean);
  const name = parts.length ? parts[parts.length - 1] : view.workspace;
  return `Tools · ${name}`;
}

function composerPlaceholder() {
  if (listening || dictationBusy === "transcribing") return;
  if (view.analysis_job) {
    input.placeholder = "Ask about the local analysis evidence";
    return;
  }
  input.placeholder = view.tools_enabled
    ? "Ask a task — each step is shown for your approval"
    : "Message Mobius — / for commands";
}

function drawAgentDeskChip() {
  const chip = $("meta-agent-desk");
  if (!chip) return;
  const g = agentDeskGlance;
  const show = g && g.caps.agent_desk && g.placement
    && (view.tools_enabled || g.desk.window_count > 0 || g.desk.placements_pending > 0);
  chip.hidden = !show;
  if (!show) return;
  if (g.desk.placements_pending > 0) chip.textContent = "Agent desk · opening";
  else if (g.desk.window_count > 0) {
    chip.textContent = "Agent desk · " + g.desk.window_count + (g.desk.window_count === 1 ? " window" : " windows");
  } else chip.textContent = "Agent desk";
  chip.classList.toggle("agent-desk-active", g.desk.window_count > 0);
  chip.title = g.desk.is_focused ? "Agent desk is focused — click to return" : "Jump to the agent desk workspace";
}

function refreshAgentDeskGlance() {
  return invoke("agent_desk_glance")
    .then((g) => { agentDeskGlance = g; drawAgentDeskChip(); })
    .catch(() => { agentDeskGlance = null; drawAgentDeskChip(); });
}

function drawComposerMeta() {
  drawLabChips();
  drawAgentDeskChip();
  const chip = $("route-chip");
  const label = status.backend || "";
  if (!label) { chip.hidden = true; }
  else {
    chip.hidden = false;
    chip.textContent = status.left_device ? ("Left device · " + label) : label;
    chip.classList.toggle("remote", !!status.left_device);
  }
  $("meta-route").textContent = label || "On this computer";
  $("meta-route").classList.toggle("remote", !!status.left_device);
  const shownModel = modelTitles[currentModel] || currentModel;
  const modelLabel = currentModel
    ? (currentBackend !== "local" ? (currentBackend + " · " + currentModel) : shownModel)
    : (currentBackend !== "local" ? (currentBackend + " · Auto") : "Auto model");
  $("meta-model").textContent = view.analysis_job ? (view.analysis_model || "Local analysis") : modelLabel;
  $("meta-model").disabled = !!view.analysis_job;
  document.body.classList.toggle("temporary", !!view.temporary);
  $("temp-banner").hidden = !view.temporary;
  $("chat-title").disabled = !!view.temporary;
  $("chat-title").title = view.temporary ? "Temporary chats are not renamed" : "Rename chat";
  const canOrganize = !view.temporary && !!view.id && view.messages.length > 0;
  $("pin-chat").hidden = !canOrganize;
  $("project-chat").hidden = !canOrganize;
  $("pin-chat").classList.toggle("active", !!view.pinned);
  $("pin-chat").title = view.pinned ? "Unpin chat" : "Pin chat";
  $("project-chat").title = view.project ? ("Project: " + view.project) : "Move to project";
  $("meta-tools").textContent = toolsChipLabel();
  $("meta-tools").classList.toggle("tools-on", !!view.tools_enabled);
  $("meta-tools").disabled = !view.id || view.streaming || !!view.analysis_job;
  $("meta-tools").title = view.analysis_job
    ? "Tools mode is unavailable during software analysis"
    : view.tools_enabled
      ? "Tools mode is on for this chat — click to change folder or turn off"
      : "Turn on Tools mode for in-chat research and reviewed project commands";
  const context = view.context;
  $("meta-context").textContent = context?.omitted_messages ? "Context · recent only" : (view.brief ? "Context · brief" : "Context");
  $("meta-context").classList.toggle("context-limited", !!context?.omitted_messages);
  $("meta-context").disabled = !view.id;
  drawQueue();
  drawProjectFilter();
  updateJumpLatest();
  composerPlaceholder();
}

function syncSendEnabled() {
  // While streaming, Enter still queues a follow-up; keep Send enabled when there is text or attachments.
  $("send").disabled = !input.value.trim() && !pendingAttachments.length && !pendingPictures.length;
}

// A model swap in progress: "Loading Qwen2.5-VL-7B…" beside the model chips until the reply starts.
listen("model-loading", (event) => {
  const chip = $("loading-chip");
  const { model, fraction } = event.payload;
  if (fraction >= 1) { chip.hidden = true; return; }
  // The event names the file without ".gguf"; show the model's own name when it is one the library knows.
  const known = Object.entries(modelTitles).find(([file]) => file.replace(/\.gguf$/, "") === model);
  chip.textContent = "Loading " + (known ? known[1] : model) + " · " + Math.round(fraction * 100) + "%";
  chip.hidden = false;
});
listen("image-progress", () => {
  status = { ...status, backend: "On this computer", left_device: false };
  drawComposerMeta();
});

function drawChat() {
  $("analysis-evidence").hidden = !view.analysis_job;
  if (!view.streaming) $("loading-chip").hidden = true;
  $("chat-title").textContent = view.temporary ? "Temporary chat" : (view.title || "New chat");
  drawComposerMeta();
  if (reactThread) {
    reactThread.set(view.messages, view.streaming, view.context, view.tasks || []);
  } else {
    const host = $("messages");
    host.replaceChildren();
    view.messages.forEach((message, index) => host.append(messageNode(message, index, index === view.messages.length - 1)));
  }
  $("empty").hidden = view.messages.length > 0 || (view.tasks || []).length > 0;
  $("send").hidden = view.streaming;
  $("stop").hidden = !view.streaming;
  $("input").disabled = false;
  syncSendEnabled();
  const banner = $("banner");
  const text = status.inference !== "ready"
    ? "Inference is offline. It starts when needed — click to check again."
    : view.error;
  banner.textContent = text;
  banner.hidden = !text;
  banner.classList.toggle("clickable", status.inference !== "ready" && !view.error);
  banner.onclick = status.inference !== "ready" && !view.error
    ? () => invoke("chat_status").then((s) => { status = s; drawChat(); }).catch(report)
    : null;
  if (stickToBottom) scrollDown();
  if (!$("find-bar").hidden && $("find-input").value.trim()) runFind();
  queueReport();
}
function scrollDown() {
  const scroller = $("scroller");
  scroller.scrollTop = scroller.scrollHeight;
  stickToBottom = true;
  updateJumpLatest();
}
$("scroller").addEventListener("scroll", () => {
  const s = $("scroller");
  stickToBottom = s.scrollHeight - s.scrollTop - s.clientHeight < 60;
  updateJumpLatest();
});
$("jump-latest").onclick = () => scrollDown();
listen("chat-view", (event) => {
  const wasStreaming = view.streaming;
  view = event.payload;
  drawChat();
  if (wasStreaming && !view.streaming) flushQueue();
});
listen("chat-stream", (event) => {
  if (reactThread) {
    reactThread.stream(event.payload.blocks);
    if (stickToBottom) scrollDown();
    return;
  }
  const last = $("messages").lastElementChild;
  if (!last || !last.classList.contains("assistant")) return;
  drawBlocks(last.querySelector(".msg-body"), event.payload.blocks);
  if (stickToBottom) scrollDown();
});

function projectNames() {
  if (window.HelixMobiusReact) return reactProjectNames;
  const names = new Set();
  chatItems.forEach((item) => { if (item.project) names.add(item.project); });
  return Array.from(names).sort((a, b) => a.localeCompare(b));
}

// The sidebar list (search, project filter, chats) is React when the bundle is present; the hand-written drawing is the fallback.
const reactChats = window.HelixMobiusReact
  ? window.HelixMobiusReact.chats($("chat-list-host"), {
    opened: (id, query) => {
      clearAttachments();
      closeSideDrawer();
      if (!query) return;
      invoke("chat_view").then((next) => {
        if (next.id !== id) return;
        view = next;
        stickToBottom = false;
        drawChat();
        $("find-input").value = query;
        openFindBar();
      }).catch(report);
    },
    projects: (names) => { reactProjectNames = names; },
    report: (error) => report(error),
  })
  : null;
let reactProjectNames = [];

const reactContext = window.HelixMobiusReact?.context($("context-host"));
function openChatContext() {
  if (reactContext && view.id) reactContext.open(view);
  else report("Rebuild the Mobius frontend to use chat context.");
}
$("meta-context").onclick = openChatContext;

// The persona rail and the rename, project, persona and model sheets are React when the bundle is present.
const reactDialogs = window.HelixMobiusReact
  ? window.HelixMobiusReact.dialogs($("dialogs-host"), $("persona-dots"), {
    report: (error) => report(error),
    routeChanged: (backend, model) => { currentBackend = backend; currentModel = model; drawComposerMeta(); },
    closeMenu: () => closeHubMenu(),
    labChanged: (value) => { lab = value; drawComposerMeta(); },
    edited: (index, text) => {
      stickToBottom = true;
      invoke("chat_edit", { index, text })
        .then(() => invoke("chat_status").then((s) => { status = s; drawComposerMeta(); }))
        .catch(report);
    },
  })
  : null;

// The message thread is React when the bundle is present; the hand-written drawing is the fallback.
const reactThread = window.HelixMobiusReact
  ? window.HelixMobiusReact.thread($("messages"), {
    edit: (index, text) => editUserMessage(index, text),
    act: (text) => actOnDesktop(text),
    quote: (text) => {
      const prefix = input.value && !input.value.endsWith("\n") ? "\n\n" : "";
      input.value = input.value + prefix + text;
      grow();
      syncSendEnabled();
      input.focus();
    },
    fork: (index) => forkChat(index),
    report: (error) => report(error),
    undo: (id) => invoke("chat_act_undo", { task: id })
      .then(() => { if (reactThread) reactThread.undo(id, false); })
      .catch((error) => {
        if (reactThread) reactThread.undo(id, false);
        report(error);
      }),
  })
  : null;
if (reactAct && reactThread) {
  listen("act-state", (event) => {
    const state = event.payload;
    if (state && state.mine && state.task) reactThread.undo(state.task, !!state.undoable);
  });
}

function drawProjectFilter() {
  if (reactChats) return;
  const host = $("project-filter");
  const names = projectNames();
  if (!names.length && !projectFilter) {
    host.hidden = true;
    host.replaceChildren();
    return;
  }
  host.hidden = false;
  host.replaceChildren();
  const all = el("button", "project-pill" + (!projectFilter ? " active" : ""), "All");
  all.type = "button";
  all.onclick = () => { projectFilter = ""; drawList(); drawComposerMeta(); };
  host.append(all);
  names.forEach((name) => {
    const pill = el("button", "project-pill" + (projectFilter === name ? " active" : ""), name);
    pill.type = "button";
    pill.onclick = () => { projectFilter = name; drawList(); drawComposerMeta(); };
    host.append(pill);
  });
}

function drawList(items) {
  if (items) chatItems = items;
  if (reactChats) { reactChats.set(chatItems, view.id); return; }
  const host = $("chats");
  host.replaceChildren();
  const needle = chatFilter.trim().toLowerCase();
  let filtered;
  const snippetById = {};
  if (needle.length >= 2 && Array.isArray(searchHits)) {
    filtered = searchHits.map((hit) => {
      snippetById[hit.id] = hit.snippet || "";
      return {
        id: hit.id,
        title: hit.title,
        updated: hit.updated,
        pinned: hit.pinned,
        project: hit.project || "",
        preview: hit.snippet || "",
      };
    });
  } else {
    filtered = chatItems;
    if (projectFilter) filtered = filtered.filter((item) => item.project === projectFilter);
    if (needle) {
      filtered = filtered.filter((item) =>
        (item.title || "").toLowerCase().includes(needle)
        || (item.project || "").toLowerCase().includes(needle)
        || (item.preview || "").toLowerCase().includes(needle));
    }
  }
  if (projectFilter && needle.length >= 2 && Array.isArray(searchHits)) {
    filtered = filtered.filter((item) => item.project === projectFilter);
  }
  if (!filtered.length) {
    host.append(el("div", "chat-empty", needle || projectFilter ? "No matching chats" : "No chats yet"));
  }
  filtered.forEach((item, index) => {
    const row = el("div", "chat-row" + (item.id === view.id ? " current" : "") + (item.pinned ? " pinned" : ""));
    row.dataset.test = "chat-" + index;
    const open = el("button", "chat-open");
    const title = el("span", "chat-open-title", item.title);
    open.append(title);
    if (item.project) open.append(el("span", "chat-open-project", item.project));
    const snippet = snippetById[item.id] || (needle && item.preview) || "";
    if (snippet && needle) open.append(el("span", "chat-open-snippet", snippet));
    if (item.pinned) {
      const mark = el("span", "chat-pin-mark");
      mark.append(icon("pin"));
      open.prepend(mark);
    }
    open.onclick = () => {
      clearAttachments();
      invoke("chat_open", { id: item.id }).catch(report);
      closeSideDrawer();
    };
    const del = el("button", "chat-delete");
    del.dataset.test = "chat-delete-" + index;
    del.title = "Delete";
    del.setAttribute("aria-label", "Delete chat");
    del.append(icon("trash"));
    del.onclick = (event) => { event.stopPropagation(); invoke("chat_delete", { id: item.id }).catch(report); };
    row.append(open, del);
    host.append(row);
  });
  drawProjectFilter();
  queueReport();
}
listen("chat-list", (event) => drawList(event.payload));
$("chat-search").addEventListener("input", () => {
  if (reactChats) return;
  chatFilter = $("chat-search").value;
  const needle = chatFilter.trim();
  clearTimeout(searchTimer);
  if (needle.length < 2) {
    searchHits = null;
    drawList();
    return;
  }
  searchTimer = setTimeout(() => {
    invoke("chat_search", { query: needle })
      .then((hits) => { if (chatFilter.trim() !== needle) return; searchHits = Array.isArray(hits) ? hits : []; drawList(); })
      .catch(() => { if (chatFilter.trim() !== needle) return; searchHits = null; drawList(); });
  }, 160);
});
$("pin-chat").onclick = () => {
  invoke("chat_pin", { pinned: !view.pinned }).catch(report);
};
$("project-chat").onclick = () => {
  if (reactDialogs) { reactDialogs.openProject(view.project || "", projectNames()); return; }
  $("project-name").value = view.project || "";
  const list = $("project-suggestions");
  list.replaceChildren(...projectNames().map((name) => {
    const opt = document.createElement("option");
    opt.value = name;
    return opt;
  }));
  $("project-dialog").showModal();
  $("project-name").focus();
};
$("project-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || (submitter.value !== "save" && submitter.value !== "clear")) return;
  event.preventDefault();
  const project = submitter.value === "clear" ? "" : $("project-name").value.trim();
  invoke("chat_set_project", { project })
    .then(() => $("project-dialog").close())
    .catch(report);
});
$("chat-title").onclick = () => {
  if (view.temporary) return;
  if (reactDialogs) { reactDialogs.openRename(view.title || "New chat"); return; }
  $("rename-title").value = view.title || "New chat";
  $("rename-dialog").showModal();
  $("rename-title").focus();
  $("rename-title").select();
};
$("rename-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || submitter.value !== "save") return;
  event.preventDefault();
  const title = $("rename-title").value.trim();
  if (!title) { report("Give the chat a title."); return; }
  invoke("chat_rename", { title })
    .then(() => $("rename-dialog").close())
    .catch(report);
});

function activePersona() {
  return (personas.personas || []).find((p) => p.id === personas.active) || (personas.personas || [])[0];
}

function personaInitials(name) {
  const parts = String(name || "").trim().split(/\s+/).filter(Boolean);
  if (parts.length >= 2) return (parts[0][0] + parts[1][0]).toUpperCase();
  return (parts[0] || "?").slice(0, 2).toUpperCase();
}

function personaHue(id) {
  let hash = 0;
  for (let i = 0; i < id.length; i++) hash = ((hash << 5) - hash + id.charCodeAt(i)) | 0;
  return Math.abs(hash) % 360;
}

function startNewPersona() {
  if (reactDialogs) { reactDialogs.openPersona(true); return; }
  const stamp = Date.now().toString(36).slice(-6);
  fillPersonaForm({
    id: "persona-" + stamp,
    name: "New persona",
    system_prompt: "",
    task: "chat",
    backend: "",
    model: "",
  });
  closeHubMenu();
  $("persona-dialog").showModal();
  $("persona-edit-name").focus();
  $("persona-edit-name").select();
}

function drawPersonaDots() {
  const host = $("persona-dots");
  host.replaceChildren();
  (personas.personas || []).forEach((p, index) => {
    const item = el("button", "persona-dot" + (p.id === personas.active ? " active" : ""));
    item.type = "button";
    item.setAttribute("role", "option");
    item.setAttribute("aria-selected", p.id === personas.active ? "true" : "false");
    item.dataset.test = "persona-dot-" + index;
    item.title = p.name + " — click to use, double-click to edit";
    item.style.setProperty("--dot-hue", String(personaHue(p.id)));
    const face = el("span", "persona-dot-face", personaInitials(p.name));
    const label = el("span", "persona-dot-label", p.name);
    item.append(face, label);
    item.onclick = () => {
      if (p.id === personas.active) {
        openPersonaSheet(p);
        return;
      }
      invoke("personas_set_active", { id: p.id }).catch(report);
    };
    item.ondblclick = (event) => {
      event.preventDefault();
      openPersonaSheet(p);
    };
    host.append(item);
  });
  const add = el("button", "persona-dot persona-dot-add");
  add.type = "button";
  add.dataset.test = "persona-dot-add";
  add.title = "New persona";
  add.setAttribute("aria-label", "New persona");
  const face = el("span", "persona-dot-face");
  face.append(icon("plus"));
  add.append(face, el("span", "persona-dot-label", "New"));
  add.onclick = () => startNewPersona();
  host.append(add);
}

function drawPersonas(store) {
  personas = store;
  if (reactDialogs) { drawComposerMeta(); return; } // the rail and sheets draw their own
  const pick = $("persona-pick");
  const previous = pick.value || editingPersonaId;
  pick.replaceChildren();
  (store.personas || []).forEach((p) => {
    const opt = document.createElement("option");
    opt.value = p.id;
    opt.textContent = p.name;
    if (p.id === (previous || store.active)) opt.selected = true;
    pick.append(opt);
  });
  if (!pick.value && store.active) pick.value = store.active;
  editingPersonaId = pick.value || store.active;
  drawPersonaDots();
  drawComposerMeta();
}
listen("personas-view", (event) => drawPersonas(event.payload));

function fillPersonaForm(persona) {
  if (!persona) return;
  editingPersonaId = persona.id;
  $("persona-edit-name").value = persona.name || "";
  $("persona-edit-prompt").value = persona.system_prompt || "";
  $("persona-edit-task").value = persona.task || "chat";
  $("persona-edit-backend").value = persona.backend || "";
  $("persona-edit-model").value = persona.model || "";
  $("persona-pick").value = persona.id;
}

function openPersonaSheet(persona) {
  if (reactDialogs) { reactDialogs.openPersona(false); return; }
  fillPersonaForm(persona || activePersona());
  closeHubMenu();
  $("persona-dialog").showModal();
  $("persona-edit-name").focus();
}

$("persona-pick").onchange = () => {
  const next = (personas.personas || []).find((p) => p.id === $("persona-pick").value);
  fillPersonaForm(next);
};
$("persona-new").onclick = () => startNewPersona();
$("persona-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || submitter.value !== "save") return;
  event.preventDefault();
  const persona = {
    id: editingPersonaId || ("persona-" + Date.now().toString(36).slice(-6)),
    name: $("persona-edit-name").value.trim(),
    system_prompt: $("persona-edit-prompt").value,
    task: $("persona-edit-task").value || "chat",
    backend: $("persona-edit-backend").value.trim(),
    model: $("persona-edit-model").value.trim(),
  };
  if (!persona.name) { report("Give the persona a name."); return; }
  invoke("personas_upsert", { persona })
    .then(() => invoke("personas_set_active", { id: persona.id }))
    .then(() => $("persona-dialog").close())
    .catch(report);
});

// A quiet message in the banner (a confirmation, a summary), cleared after a while.
function info(text, ms = 5000) {
  const banner = $("banner");
  banner.textContent = text;
  banner.hidden = false;
  clearTimeout(info.timer);
  info.timer = setTimeout(() => { if (banner.textContent === text) banner.hidden = true; }, ms);
}

function report(error) {
  const banner = $("banner");
  banner.textContent = String(error);
  banner.hidden = false;
}

const input = $("input");
function grow() {
  input.style.height = "auto";
  input.style.height = Math.min(180, input.scrollHeight) + "px";
  syncSendEnabled();
}
input.addEventListener("input", () => {
  grow();
  maybeSlashFromInput();
  if (promptHistoryIndex >= 0 && input.value !== promptHistory[promptHistoryIndex]) {
    promptHistoryIndex = -1;
    promptDraft = "";
  }
});
input.addEventListener("keydown", (event) => {
  const slashOpen = !$("slash-menu").hidden;
  if (slashOpen && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
    event.preventDefault();
    const items = filteredSlash(input.value.trim().split(/\s+/)[0].slice(1));
    if (!items.length) return;
    slashIndex = event.key === "ArrowDown"
      ? (slashIndex + 1) % items.length
      : (slashIndex - 1 + items.length) % items.length;
    showSlashMenu(input.value.trim().split(/\s+/)[0].slice(1));
    return;
  }
  if (slashOpen && event.key === "Tab") {
    event.preventDefault();
    const items = filteredSlash(input.value.trim().split(/\s+/)[0].slice(1));
    if (items[slashIndex]) runSlash(items[slashIndex]);
    return;
  }
  if (slashOpen && event.key === "Escape") {
    event.preventDefault();
    hideSlashMenu();
    return;
  }
  if (!slashOpen && (event.key === "ArrowUp" || event.key === "ArrowDown") && !event.shiftKey && !event.altKey && !event.metaKey && !event.ctrlKey) {
    const atStart = input.selectionStart === 0 && input.selectionEnd === 0;
    const empty = !input.value;
    if ((empty || atStart) && promptHistory.length) {
      event.preventDefault();
      if (event.key === "ArrowUp") {
        if (promptHistoryIndex < 0) promptDraft = input.value;
        promptHistoryIndex = Math.min(promptHistoryIndex + 1, promptHistory.length - 1);
        input.value = promptHistory[promptHistoryIndex] || "";
      } else if (promptHistoryIndex >= 0) {
        promptHistoryIndex -= 1;
        input.value = promptHistoryIndex < 0 ? promptDraft : (promptHistory[promptHistoryIndex] || "");
      }
      grow();
      input.setSelectionRange(input.value.length, input.value.length);
      return;
    }
  }
  if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    if (slashOpen) {
      const items = filteredSlash(input.value.trim().split(/\s+/)[0].slice(1));
      if (items[slashIndex]) { runSlash(items[slashIndex]); return; }
    }
    $("composer").requestSubmit();
  }
});
$("composer").addEventListener("submit", (event) => {
  event.preventDefault();
  hideSlashMenu();
  const text = input.value;
  if (!text.trim() && !pendingAttachments.length && !pendingPictures.length) return;
  if (text.trim().startsWith("/") && tryRunSlashSubmit()) return;
  if (view.streaming) {
    if (!text.trim() || pendingPictures.length || pendingAttachments.length) { report("Wait for the reply, or stop it, before sending attachments."); return; }
    if (queue.length >= MAX_QUEUE) { report(`${MAX_QUEUE} follow-ups are already waiting.`); return; }
    queue.push(text.trim());
    rememberPrompt(text);
    input.value = "";
    grow();
    drawQueue();
    return;
  }
  const attachments = attachmentPayload();
  const pictures = pendingPictures.map((p) => p.id);
  rememberPrompt(text);
  input.value = "";
  grow();
  stickToBottom = true;
  invoke("chat_send", { text, attachments: attachments.length ? attachments : null, pictures: pictures.length ? pictures : null })
    .then(() => {
      clearAttachments();
      clearPictures();
      return invoke("chat_status").then((s) => { status = s; drawComposerMeta(); });
    })
    .catch((error) => { input.value = text; grow(); report(error); });
});
$("stop").onclick = () => invoke("chat_stop");
$("attach").onclick = () => pickAttachments();
$("attach-picture").onclick = () => pickPictures();
input.addEventListener("paste", (event) => {
  const files = event.clipboardData && event.clipboardData.files;
  if (files && files.length && addPictureFiles(files)) event.preventDefault();
});
$("act").onclick = () => {
  const typed = input.value.trim();
  const text = typed || (view.messages.slice().reverse().find((m) => m.role === "user") || {}).text || "";
  if (!text.trim()) { report("Write something to act on."); return; }
  // Handed over like a sent message: the draft goes, unless it was changed meanwhile or the hand-over failed.
  actOnDesktop(text, typed ? () => {
    if (input.value.trim() === typed) { input.value = ""; grow(); syncSendEnabled(); }
  } : null);
};
$("new-chat").onclick = () => {
  invoke("chat_new").then(() => { clearAttachments(); input.focus(); closeSideDrawer(); }).catch(report);
};
$("temp-chat").onclick = () => {
  invoke("chat_temporary").then(() => { clearAttachments(); input.focus(); closeSideDrawer(); }).catch(report);
};
$("fork-chat").onclick = () => forkChat();
$("find-input").addEventListener("input", runFind);
$("find-input").addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    event.preventDefault();
    if (event.shiftKey) focusFindMatch(findIndex - 1);
    else focusFindMatch(findIndex + 1);
  }
  if (event.key === "Escape") {
    event.preventDefault();
    closeFindBar();
    input.focus();
  }
});
$("find-prev").onclick = () => focusFindMatch(findIndex - 1);
$("find-next").onclick = () => focusFindMatch(findIndex + 1);
$("find-close").onclick = () => { closeFindBar(); input.focus(); };
$("meta-model").onclick = () => {
  // A small menu above the chip: one click switches. The full sheet (other backends, a custom id) is its last row.
  if (!reactDialogs) { openModelPicker().catch(report); return; }
  const box = $("meta-model").getBoundingClientRect();
  reactDialogs.openMenu("model", { left: box.left, top: box.top, width: box.width });
};
$("meta-persona").onclick = () => openMeta("persona");
$("meta-style").onclick = () => openMeta("style");

async function fillModelList(backend) {
  const host = $("model-list");
  host.replaceChildren();
  const auto = el("button", "model-option" + (!currentModel ? " active" : ""), "Auto (default)");
  auto.type = "button";
  auto.onclick = () => {
    $("model-custom").value = "";
    host.querySelectorAll(".model-option").forEach((n) => n.classList.remove("active"));
    auto.classList.add("active");
  };
  host.append(auto);
  let models = [];
  if (backend === "local") {
    models = await invoke("routing_list_models");
  } else {
    models = await invoke("routing_list_remote_models", { backendId: backend });
  }
  (Array.isArray(models) ? models : []).forEach((id) => {
    const row = el("button", "model-option" + (currentModel === id ? " active" : ""), id);
    row.type = "button";
    row.onclick = () => {
      $("model-custom").value = id;
      host.querySelectorAll(".model-option").forEach((n) => n.classList.remove("active"));
      row.classList.add("active");
    };
    host.append(row);
  });
}

async function openModelPicker() {
  if (reactDialogs) { reactDialogs.openModel(); return; }
  const route = await invoke("chat_current_route");
  currentModel = (route && route.model) || "";
  currentBackend = (route && route.backend) || "local";
  $("model-custom").value = currentModel || "";
  const select = $("model-backend");
  if (![...select.options].some((o) => o.value === currentBackend)) {
    const opt = document.createElement("option");
    opt.value = currentBackend;
    opt.textContent = (route && route.label) || currentBackend;
    select.append(opt);
  }
  select.value = currentBackend;
  if (currentBackend === "openrouter") {
    await invoke("routing_ensure_openrouter").catch(() => {});
  }
  await fillModelList(select.value);
  $("model-dialog").showModal();
}
$("model-backend").onchange = () => {
  const backend = $("model-backend").value;
  const ensure = backend === "openrouter"
    ? invoke("routing_ensure_openrouter")
    : Promise.resolve();
  ensure.then(() => fillModelList(backend)).catch(report);
};
$("model-form").addEventListener("submit", (event) => {
  const submitter = event.submitter;
  if (!submitter || (submitter.value !== "save" && submitter.value !== "auto")) return;
  event.preventDefault();
  const backend = $("model-backend").value || "local";
  const model = submitter.value === "auto" ? "" : $("model-custom").value.trim();
  const ensure = backend === "openrouter"
    ? invoke("routing_ensure_openrouter")
    : Promise.resolve();
  ensure
    .then(() => invoke("chat_set_route", { backend, model }))
    .then(() => {
      currentBackend = backend;
      currentModel = model;
      $("model-dialog").close();
      drawComposerMeta();
    })
    .catch(report);
});
$("export-chat").onclick = () => {
  closeHubMenu();
  invoke("chat_export_markdown")
    .then((markdown) => invoke("chat_copy", { text: markdown }))
    .then(() => {
      const banner = $("banner");
      banner.textContent = "Copied conversation as Markdown.";
      banner.hidden = false;
      setTimeout(() => { if (banner.textContent.startsWith("Copied conversation")) banner.hidden = true; }, 2500);
    })
    .catch(report);
};

// Hub menu --------------------------------------------------------------------------------------------------------------------------
function closeHubMenu() {
  $("hub-menu").hidden = true;
  $("hub-toggle").setAttribute("aria-expanded", "false");
}
function toggleHubMenu() {
  const open = $("hub-menu").hidden;
  $("hub-menu").hidden = !open;
  $("hub-toggle").setAttribute("aria-expanded", open ? "true" : "false");
}
$("hub-toggle").onclick = (event) => { event.stopPropagation(); toggleHubMenu(); };
document.addEventListener("click", (event) => {
  if (!$("hub-menu").hidden && !$("side-footer").contains(event.target)) closeHubMenu();
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    if (!$("find-bar").hidden) {
      closeFindBar();
      return;
    }
    if (!$("slash-menu").hidden) {
      hideSlashMenu();
      return;
    }
    if (queue.length) {
      queue = [];
      drawQueue();
      return;
    }
    if (view.streaming) invoke("chat_stop");
    closeHubMenu();
    closeSideDrawer();
    return;
  }
  const meta = event.metaKey || event.ctrlKey;
  if (meta && event.key.toLowerCase() === "n" && !event.shiftKey) {
    event.preventDefault();
    $("new-chat").click();
  }
  if (meta && event.shiftKey && event.key.toLowerCase() === "n") {
    event.preventDefault();
    $("temp-chat").click();
  }
  if (meta && event.key.toLowerCase() === "k") {
    event.preventDefault();
    openSideDrawer();
    $("chat-search").focus();
    $("chat-search").select();
  }
  if (meta && event.shiftKey && event.key.toLowerCase() === "c") {
    event.preventDefault();
    $("export-chat").click();
  }
  if (meta && event.key.toLowerCase() === "f" && !event.shiftKey) {
    event.preventDefault();
    openFindBar();
  }
  if (meta && event.shiftKey && event.key.toLowerCase() === "f") {
    event.preventDefault();
    forkChat();
  }
  if (meta && event.key.toLowerCase() === "u") {
    event.preventDefault();
    pickAttachments();
  }
  if (meta && event.key === "/") {
    event.preventDefault();
    input.focus();
    input.value = "/";
    grow();
    showSlashMenu("");
  }
  if (meta && event.key.toLowerCase() === "l" && !event.shiftKey) {
    event.preventDefault();
    input.focus();
    const end = input.value.length;
    input.setSelectionRange(end, end);
  }
  if (meta && event.key.toLowerCase() === "m" && !event.shiftKey) {
    event.preventDefault();
    openRouting();
  }
  if (meta && event.shiftKey && event.key.toLowerCase() === "a") {
    event.preventDefault();
    const text = (window.getSelection() && String(window.getSelection())) || lastActableText();
    if (!text.trim()) { report("Nothing to act on yet."); return; }
    actOnDesktop(text.trim());
  }
  if (meta && event.key === "Enter" && event.shiftKey) {
    event.preventDefault();
    const draft = input.value.trim() || lastActableText();
    if (!draft) { report("Write something to act on."); return; }
    actOnDesktop(draft);
  }
});

// Drag files onto the composer to attach.
["dragenter", "dragover"].forEach((name) => {
  $("composer").addEventListener(name, (event) => {
    event.preventDefault();
    $("composer").classList.add("drop-target");
  });
});
["dragleave", "drop"].forEach((name) => {
  $("composer").addEventListener(name, (event) => {
    event.preventDefault();
    if (name === "dragleave" && $("composer").contains(event.relatedTarget)) return;
    $("composer").classList.remove("drop-target");
  });
});
$("composer").addEventListener("drop", (event) => {
  event.preventDefault();
  $("composer").classList.remove("drop-target");
  const files = event.dataTransfer && event.dataTransfer.files;
  if (!files || !files.length) return;
  if (addPictureFiles(files)) return;
  // File paths from the OS drop are not readable in the webview; open the native picker instead.
  pickAttachments();
});

$("open-assistant").onclick = () => { closeHubMenu(); invoke("chat_open_shell_panel", { id: "assistant" }).catch(report); };
$("open-agents").onclick = () => { closeHubMenu(); invoke("chat_open_shell_panel", { id: "agents" }).catch(report); };
$("open-settings-agents").onclick = () => { closeHubMenu(); invoke("open_settings_agents").catch(report); };

// Narrow sidebar drawer -------------------------------------------------------------------------------------------------------------
function closeSideDrawer() {
  document.body.classList.remove("side-open");
  $("side-toggle").setAttribute("aria-expanded", "false");
  $("side-scrim").hidden = true;
}
function openSideDrawer() {
  document.body.classList.add("side-open");
  $("side-toggle").setAttribute("aria-expanded", "true");
  $("side-scrim").hidden = false;
}
$("side-toggle").onclick = () => {
  if (document.body.classList.contains("side-open")) closeSideDrawer();
  else openSideDrawer();
};
$("side-scrim").onclick = () => closeSideDrawer();
window.matchMedia("(min-width: 720px)").addEventListener("change", (event) => {
  if (event.matches) closeSideDrawer();
});

// Models page -----------------------------------------------------------------------------------------------------------------------
const ROUTE_SOURCES = [
  { id: "local", label: "My computer", short: "This PC", kind: "local" },
  { id: "lan", label: "LAN", short: "LAN", kind: "remote" },
  { id: "openrouter", label: "Cloud", short: "Cloud", kind: "remote" },
];
const ROUTE_TASKS = [
  { id: "chat", kicker: "Talk", title: "Chat", locked: false },
  { id: "act", kicker: "Desktop", title: "Act", locked: true },
  { id: "code", kicker: "Dev", title: "Code", locked: false },
  { id: "summarize", kicker: "Quick", title: "Summarize", locked: false },
  { id: "vision", kicker: "Sight", title: "Vision", locked: false },
];

let routeDraft = null;
let routeSource = "local";
let routeModelCache = { local: [], lan: [], openrouter: [] };
let routeModelsLoading = false;
let modelsSection = "overview";
let modelsDirty = false;

function sourceLabel(id) {
  return (ROUTE_SOURCES.find((s) => s.id === id) || {}).short || id || "This PC";
}

function attachModelList(ids) {
  const listId = "local-gguf-models";
  let datalist = document.getElementById(listId);
  if (!datalist) {
    datalist = document.createElement("datalist");
    datalist.id = listId;
    document.body.appendChild(datalist);
  }
  datalist.replaceChildren(...(ids || []).map((id) => {
    const option = document.createElement("option");
    option.value = id;
    return option;
  }));
  const personaModel = $("persona-edit-model");
  if (personaModel) personaModel.setAttribute("list", listId);
}

function markModelsDirty() {
  modelsDirty = true;
}

function syncRouteHidden() {
  if (!routeDraft) return;
  $("route-chat-model").value = routeDraft.profiles.chat.model || "";
  $("route-act-model").value = routeDraft.profiles.act.model || "";
  $("route-code-model").value = routeDraft.profiles.code.model || "";
  $("route-chat-backend").value = routeDraft.profiles.chat.backend || "local";
  $("route-code-backend").value = routeDraft.profiles.code.backend || "local";
}

function profileOf(task) {
  if (!routeDraft.profiles[task]) routeDraft.profiles[task] = { backend: "local", model: "" };
  return routeDraft.profiles[task];
}

function showModelsSection(id) {
  modelsSection = id;
  document.querySelectorAll(".models-nav-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.modelsSection === id);
  });
  document.querySelectorAll(".models-panel").forEach((panel) => {
    const on = panel.dataset.modelsPanel === id;
    panel.hidden = !on;
    panel.classList.toggle("active", on);
  });
  if (id === "local") {
    routeSource = "local";
    loadRouteModels("local").catch(report);
  } else if (id === "lan") {
    routeSource = "lan";
    loadRouteModels("lan").catch(report);
  } else if (id === "cloud") {
    routeSource = "openrouter";
    loadRouteModels("openrouter").catch(report);
  }
}

function drawModelsInfo(info) {
  const host = $("models-info-cards");
  host.replaceChildren();
  const cards = [
    { kicker: "Inference", value: info && info.inference_ready ? "Ready" : "Offline", detail: (info && info.local_model_count || 0) + " local GGUF(s)", ok: !!(info && info.inference_ready), warn: !(info && info.inference_ready) },
    { kicker: "Chat route", value: sourceLabel(profileOf("chat").backend), detail: profileOf("chat").model || "Auto model", ok: true },
    { kicker: "LAN", value: info && info.lan_configured ? "Configured" : "Not set", detail: routeDraft.lanUrl || "No endpoint", ok: !!(info && info.lan_configured), warn: !(info && info.lan_configured) },
    { kicker: "Cloud", value: info && info.has_cloud_key ? "Key saved" : "No key", detail: routeDraft.cloudUrl || "OpenRouter", ok: !!(info && info.has_cloud_key), warn: !(info && info.has_cloud_key) },
  ];
  cards.forEach((c) => {
    const card = el("div", "models-info-card" + (c.ok ? " ok" : "") + (c.warn ? " warn" : ""));
    card.append(el("div", "kicker", c.kicker));
    card.append(el("div", "value", c.value));
    card.append(el("div", "detail", c.detail));
    host.append(card);
  });
  if (info && info.config_path) $("route-config-path").textContent = info.config_path;
}

function drawRouteTasks() {
  const host = $("route-tasks");
  host.replaceChildren();
  ROUTE_TASKS.forEach((task) => {
    const profile = profileOf(task.id);
    const card = el("article", "route-task" + (task.locked ? " locked" : ""));
    card.dataset.task = task.id;
    card.append(el("div", "route-task-kicker", task.kicker));
    card.append(el("div", "route-task-title", task.title));
    card.append(el("div", "route-task-dest", sourceLabel(task.locked ? "local" : profile.backend)));
    card.append(el("div", "route-task-model", profile.model || "Auto (default)"));
    if (task.locked) {
      card.append(el("p", "route-task-lock", "Local only — tools stay on-device"));
    } else {
      const picks = el("div", "route-task-picks");
      ROUTE_SOURCES.forEach((source) => {
        const btn = el("button", "route-dest-btn" + (profile.backend === source.id ? " active" : ""), source.short);
        btn.type = "button";
        btn.onclick = () => {
          profile.backend = source.id;
          if (source.id === "lan" && !routeDraft.lanUrl) showModelsSection("lan");
          if (source.id === "openrouter") showModelsSection("cloud");
          markModelsDirty();
          syncRouteHidden();
          drawModelsPage();
        };
        picks.append(btn);
      });
      card.append(picks);
    }
    host.append(card);
  });
}

function modelBadges(source, id) {
  const badges = [];
  ROUTE_TASKS.forEach((task) => {
    const profile = profileOf(task.id);
    const backend = task.locked ? "local" : profile.backend;
    if (backend === source && profile.model === id) badges.push(task.title);
  });
  return badges;
}

function fillModelGrid(hostId, emptyId, source, needle) {
  const host = $(hostId);
  const empty = $(emptyId);
  if (!host) return;
  let models = routeModelCache[source] || [];
  if (needle) models = models.filter((id) => id.toLowerCase().includes(needle));
  host.replaceChildren();
  if (routeModelsLoading && !models.length) {
    empty.hidden = false;
    empty.textContent = "Loading models…";
    return;
  }
  const autoSelected = ROUTE_TASKS.some((task) => {
    const profile = profileOf(task.id);
    const backend = task.locked ? "local" : profile.backend;
    return backend === source && !profile.model;
  });
  const auto = el("button", "route-model-card" + (autoSelected ? " selected" : ""));
  auto.type = "button";
  const autoMain = el("div");
  autoMain.append(el("div", "route-model-name", "Auto (default)"));
  autoMain.append(el("div", "route-model-meta", source === "local" ? "Inference service default" : "Provider default"));
  auto.append(autoMain);
  const autoActions = el("div", "route-model-actions");
  ROUTE_TASKS.forEach((task) => {
    if (task.locked && source !== "local") return;
    const profile = profileOf(task.id);
    const backend = task.locked ? "local" : profile.backend;
    const active = backend === source && !profile.model;
    const btn = el("button", "route-use-btn" + (active ? " active" : ""), "Use for " + task.title);
    btn.type = "button";
    btn.onclick = (event) => { event.stopPropagation(); assignRouteModel(task.id, source, ""); };
    autoActions.append(btn);
  });
  auto.append(autoActions);
  auto.onclick = () => assignRouteModel("chat", source, "");
  host.append(auto);
  models.forEach((id) => {
    const badges = modelBadges(source, id);
    const card = el("button", "route-model-card" + (badges.length ? " selected" : ""));
    card.type = "button";
    const main = el("div");
    main.append(el("div", "route-model-name", id));
    main.append(el("div", "route-model-meta",
      source === "local" ? "Local GGUF" : (source === "lan" ? "LAN · OpenAI-compatible" : "OpenRouter")));
    card.append(main);
    if (badges.length) {
      const badgeHost = el("div", "route-model-badges");
      badges.forEach((b) => badgeHost.append(el("span", "route-badge", b)));
      card.append(badgeHost);
    }
    const actions = el("div", "route-model-actions");
    ROUTE_TASKS.forEach((task) => {
      if (task.locked && source !== "local") return;
      const active = badges.includes(task.title);
      const btn = el("button", "route-use-btn" + (active ? " active" : ""), "Use for " + task.title);
      btn.type = "button";
      btn.onclick = (event) => { event.stopPropagation(); assignRouteModel(task.id, source, id); };
      actions.append(btn);
    });
    card.append(actions);
    card.onclick = () => assignRouteModel("chat", source, id);
    host.append(card);
  });
  empty.hidden = models.length > 0 || routeModelsLoading;
  if (!models.length && !routeModelsLoading) {
    empty.textContent = source === "local"
      ? "No local GGUFs yet. Inference lists them when available."
      : (source === "lan"
        ? "Set a LAN URL, then Refresh / Test connection."
        : "Add an API key, then Test connection (suggestions appear if catalog is offline).");
  }
}

function drawRouteModels() {
  const needle = ($("route-model-search-local") && $("route-model-search-local").value || "").trim().toLowerCase();
  fillModelGrid("route-model-grid", "route-model-empty", "local", needle);
  fillModelGrid("route-lan-model-grid", "route-lan-model-empty", "lan", "");
  fillModelGrid("route-cloud-model-grid", "route-cloud-model-empty", "openrouter", "");
}

function assignRouteModel(task, source, modelId) {
  if (!routeDraft) return;
  if (task === "act" && source !== "local") {
    report("Act must stay on this computer.");
    return;
  }
  const profile = profileOf(task);
  profile.backend = task === "act" ? "local" : source;
  profile.model = modelId;
  markModelsDirty();
  syncRouteHidden();
  drawModelsPage();
}

function drawGenerationFields() {
  if (!routeDraft) return;
  $("route-temperature").value = String(routeDraft.temperature);
  $("route-temperature-value").textContent = Number(routeDraft.temperature).toFixed(2);
  $("route-max-tokens").value = String(routeDraft.maxTokens);
  $("route-timeout").value = String(routeDraft.timeoutSecs);
  $("route-battery").checked = !!routeDraft.battery;
}

async function drawModelsPage(info) {
  drawRouteTasks();
  drawRouteModels();
  drawGenerationFields();
  if (info) drawModelsInfo(info);
  else {
    try {
      const page = await invoke("routing_page_info");
      drawModelsInfo(page);
    } catch (_) { /* ignore */ }
  }
}

async function refreshCloudKeyStatus() {
  const keyStatus = await invoke("routing_secret_status", { apiKeyRef: "openrouter" });
  const has = !!(keyStatus && keyStatus.has_key);
  if (routeDraft) routeDraft.hasCloudKey = has;
  $("route-cloud-key-status").textContent = has
    ? "API key is saved securely."
    : "No API key saved yet.";
}

async function loadRouteModels(source) {
  routeModelsLoading = true;
  drawRouteModels();
  try {
    if (source === "local") {
      const models = await invoke("routing_list_models");
      routeModelCache.local = Array.isArray(models) ? models : [];
      attachModelList(routeModelCache.local);
    } else {
      if (source === "openrouter") await invoke("routing_ensure_openrouter").catch(() => {});
      // Persist draft URL into routing before listing if needed — list uses saved backends.
      // For LAN/cloud draft URLs not yet saved, still try via ensure + current saved config.
      const models = await invoke("routing_list_remote_models", { backendId: source });
      routeModelCache[source] = Array.isArray(models) ? models : [];
    }
  } catch (_) {
    routeModelCache[source] = routeModelCache[source] || [];
  } finally {
    routeModelsLoading = false;
    drawRouteModels();
  }
}

function closeModelsPage() {
  document.body.classList.remove("models-open");
  $("models-page").hidden = true;
  modelsDirty = false;
}

// The Models & routing page is React when the bundle is present; the hand-written page below is the fallback.
const reactModels = window.HelixMobiusReact
  ? window.HelixMobiusReact.models($("models-page"), {
    visible: (shown) => {
      document.body.classList.toggle("models-open", shown);
      $("models-page").hidden = !shown;
      queueReport();
    },
    saved: (backend, model) => {
      currentBackend = backend;
      currentModel = model;
      invoke("chat_status").then((next) => { status = next; drawComposerMeta(); });
    },
    localModels: (ids) => attachModelList(ids),
    report: (error) => report(error),
  })
  : null;

const reactWeb = window.HelixMobiusReact?.web
  ? window.HelixMobiusReact.web($("web-host"), () => { stickToBottom = true; }) : null;
function openWeb(query) {
  closeHubMenu(); closeSideDrawer();
  if (reactWeb) reactWeb.open(view, query);
  else report("Build the Mobius frontend to look online.");
}
$("open-web").onclick = () => openWeb();

const reactAnalysis = window.HelixMobiusReact?.analysis
  ? window.HelixMobiusReact.analysis($("analysis-host"), report)
  : null;
const reactCreateImage = window.HelixMobiusReact?.createImage
  ? window.HelixMobiusReact.createImage($("create-image-host")) : null;
function openCreateImage() {
  closeHubMenu(); closeSideDrawer();
  if (reactCreateImage) reactCreateImage.open();
  else report("Build the Mobius frontend to create images.");
}
$("open-create-image").onclick = openCreateImage;
window.addEventListener("mobius-image-prompt", (event) => {
  if (typeof event.detail !== "string") return;
  if (input.value.trim()) { reactCreateImage?.open(event.detail); return; }
  input.value = "/image " + event.detail;
  grow(); syncSendEnabled(); maybeSlashFromInput(); input.focus();
});
$("open-analysis").onclick = () => {
  closeHubMenu();
  closeSideDrawer();
  if (reactAnalysis) reactAnalysis.open();
  else report("Build the Mobius frontend to use software analysis.");
};
$("analysis-evidence").onclick = () => reactAnalysis?.open(view.analysis_job);

async function openRouting() {
  closeHubMenu();
  closeSideDrawer();
  if (reactModels) { reactModels.open(); return; }
  await invoke("routing_ensure_openrouter").catch(() => {});
  const routing = await invoke("routing_get");
  const lan = (routing.backends || []).find((b) => b.id === "lan");
  const cloud = (routing.backends || []).find((b) => b.id === "openrouter");
  const profiles = {};
  ["chat", "act", "summarize", "vision", "code"].forEach((name) => {
    const p = (routing.profiles && routing.profiles[name]) || { backend: "local", model: "" };
    profiles[name] = {
      backend: name === "act" ? "local" : (["local", "lan", "openrouter"].includes(p.backend) ? p.backend : "local"),
      model: p.model || "",
    };
  });
  const talk = routing.talk || {};
  routeDraft = {
    profiles,
    lanUrl: lan ? (lan.base_url || "") : "",
    cloudUrl: cloud ? (cloud.base_url || "https://openrouter.ai/api/v1") : "https://openrouter.ai/api/v1",
    battery: !!routing.allow_remote_on_battery_saver,
    temperature: talk.temperature != null ? talk.temperature : 0.6,
    maxTokens: talk.max_tokens != null ? talk.max_tokens : 1024,
    timeoutSecs: talk.remote_timeout_secs != null ? talk.remote_timeout_secs : 120,
    hasCloudKey: false,
  };
  routeSource = profiles.chat.backend || "local";
  $("route-lan-url").value = routeDraft.lanUrl;
  $("route-cloud-url").value = routeDraft.cloudUrl;
  $("route-cloud-key").value = "";
  if ($("route-model-search-local")) $("route-model-search-local").value = "";
  $("route-probe-lan-status").textContent = "";
  $("route-probe-cloud-status").textContent = "";
  syncRouteHidden();
  await refreshCloudKeyStatus().catch(() => { $("route-cloud-key-status").textContent = ""; });
  const info = await invoke("routing_page_info").catch(() => null);
  document.body.classList.add("models-open");
  $("models-page").hidden = false;
  showModelsSection("overview");
  await drawModelsPage(info);
  modelsDirty = false;
  loadRouteModels("local").catch(report);
}

async function saveModelsPage() {
  if (!routeDraft) return;
  const lanUrl = $("route-lan-url").value.trim();
  const cloudUrl = $("route-cloud-url").value.trim() || "https://openrouter.ai/api/v1";
  routeDraft.lanUrl = lanUrl;
  routeDraft.cloudUrl = cloudUrl;
  routeDraft.battery = $("route-battery").checked;
  routeDraft.temperature = Number($("route-temperature").value);
  routeDraft.maxTokens = Number($("route-max-tokens").value);
  routeDraft.timeoutSecs = Number($("route-timeout").value);
  if (routeDraft.profiles.chat.backend === "lan" && !lanUrl) {
    report("Set a LAN URL before routing Chat there.");
    showModelsSection("lan");
    return;
  }
  if (routeDraft.profiles.code.backend === "lan" && !lanUrl) {
    report("Set a LAN URL before routing Code there.");
    showModelsSection("lan");
    return;
  }
  const cloudKey = $("route-cloud-key").value.trim();
  const keyStep = cloudKey
    ? invoke("routing_set_secret", { apiKeyRef: "openrouter", secret: cloudKey })
    : Promise.resolve();
  await keyStep;
  const routing = await invoke("routing_get");
  if (!routing.profiles) routing.profiles = {};
  if (!routing.backends) routing.backends = [];
  ["act", "chat", "summarize", "vision", "code"].forEach((name) => {
    if (!routing.profiles[name]) routing.profiles[name] = { backend: "local", model: "" };
    routing.profiles[name].backend = routeDraft.profiles[name].backend || "local";
    routing.profiles[name].model = routeDraft.profiles[name].model || "";
  });
  routing.profiles.act.backend = "local";
  routing.allow_remote_on_battery_saver = !!routeDraft.battery;
  routing.talk = {
    temperature: routeDraft.temperature,
    max_tokens: routeDraft.maxTokens,
    remote_timeout_secs: routeDraft.timeoutSecs,
  };
  let lan = routing.backends.find((b) => b.id === "lan");
  if (lanUrl) {
    if (!lan) {
      lan = { id: "lan", kind: "openai", base_url: lanUrl, label: "LAN", api_key_ref: "" };
      routing.backends.push(lan);
    } else {
      lan.kind = "openai";
      lan.base_url = lanUrl;
      if (!lan.label) lan.label = "LAN";
      if (lan.api_key_ref == null) lan.api_key_ref = "";
    }
  }
  let cloud = routing.backends.find((b) => b.id === "openrouter");
  if (!cloud) {
    cloud = { id: "openrouter", kind: "openai", base_url: cloudUrl, label: "OpenRouter", api_key_ref: "openrouter" };
    routing.backends.push(cloud);
  } else {
    cloud.kind = "openai";
    cloud.base_url = cloudUrl;
    cloud.label = cloud.label || "OpenRouter";
    cloud.api_key_ref = cloud.api_key_ref || "openrouter";
  }
  if (!routing.backends.some((b) => b.id === "local")) {
    routing.backends.unshift({ id: "local", kind: "local", base_url: "", label: "On this computer", api_key_ref: "" });
  }
  await invoke("routing_save", { routing });
  currentBackend = routeDraft.profiles.chat.backend || "local";
  currentModel = routeDraft.profiles.chat.model || "";
  status = await invoke("chat_status");
  drawComposerMeta();
  modelsDirty = false;
  closeModelsPage();
}

$("open-routing").onclick = () => openRouting().catch(report);
$("open-models").onclick = () => openRouting().catch(report);
$("route-chip").onclick = () => openRouting().catch(report);
$("meta-route").onclick = () => openRouting().catch(report);
window.addEventListener("mobius-open-models", () => openRouting().catch(report));
$("models-back").onclick = () => {
  if (modelsDirty && !window.confirm("Discard unsaved model changes?")) return;
  closeModelsPage();
};
$("models-discard").onclick = () => {
  if (modelsDirty && !window.confirm("Discard unsaved model changes?")) return;
  closeModelsPage();
};
$("models-save").onclick = () => saveModelsPage().catch(report);
document.querySelectorAll(".models-nav-btn").forEach((btn) => {
  btn.onclick = () => showModelsSection(btn.dataset.modelsSection);
});
if ($("route-model-search-local")) {
  $("route-model-search-local").addEventListener("input", () => drawRouteModels());
}
$("route-refresh-models").onclick = () => loadRouteModels(routeSource === "openrouter" || routeSource === "lan" ? routeSource : "local").catch(report);
$("route-lan-url").addEventListener("input", () => { if (routeDraft) { routeDraft.lanUrl = $("route-lan-url").value.trim(); markModelsDirty(); } });
$("route-cloud-url").addEventListener("input", () => { if (routeDraft) { routeDraft.cloudUrl = $("route-cloud-url").value.trim(); markModelsDirty(); } });
$("route-battery").addEventListener("change", () => { if (routeDraft) { routeDraft.battery = $("route-battery").checked; markModelsDirty(); } });
$("route-temperature").addEventListener("input", () => {
  $("route-temperature-value").textContent = Number($("route-temperature").value).toFixed(2);
  if (routeDraft) { routeDraft.temperature = Number($("route-temperature").value); markModelsDirty(); }
});
$("route-max-tokens").addEventListener("change", () => { if (routeDraft) { routeDraft.maxTokens = Number($("route-max-tokens").value); markModelsDirty(); } });
$("route-timeout").addEventListener("change", () => { if (routeDraft) { routeDraft.timeoutSecs = Number($("route-timeout").value); markModelsDirty(); } });
$("route-clear-key").onclick = () => {
  invoke("routing_clear_secret", { apiKeyRef: "openrouter" })
    .then(() => { $("route-cloud-key").value = ""; return refreshCloudKeyStatus(); })
    .then(() => invoke("routing_page_info").then(drawModelsInfo))
    .catch(report);
};
$("route-reset-defaults").onclick = () => {
  if (!routeDraft) return;
  routeDraft.temperature = 0.6;
  routeDraft.maxTokens = 1024;
  routeDraft.timeoutSecs = 120;
  markModelsDirty();
  drawGenerationFields();
};
$("route-probe-lan").onclick = () => {
  // Save URL into draft routing temporarily via routing_save of URL only is heavy; probe uses saved backend.
  // Persist LAN URL first into draft file by a lightweight ensure.
  const url = $("route-lan-url").value.trim();
  if (!url) { report("Enter a LAN URL first."); return; }
  invoke("routing_get").then((routing) => {
    if (!routing.backends) routing.backends = [];
    let lan = routing.backends.find((b) => b.id === "lan");
    if (!lan) {
      lan = { id: "lan", kind: "openai", base_url: url, label: "LAN", api_key_ref: "" };
      routing.backends.push(lan);
    } else {
      lan.base_url = url;
      lan.kind = "openai";
    }
    return invoke("routing_save", { routing });
  }).then(() => invoke("routing_probe", { backendId: "lan" }))
    .then((msg) => {
      const node = $("route-probe-lan-status");
      node.textContent = msg;
      node.className = "models-inline-status ok";
      return loadRouteModels("lan");
    })
    .catch((error) => {
      const node = $("route-probe-lan-status");
      node.textContent = String(error);
      node.className = "models-inline-status bad";
    });
};
$("route-probe-cloud").onclick = () => {
  const cloudKey = $("route-cloud-key").value.trim();
  const url = $("route-cloud-url").value.trim() || "https://openrouter.ai/api/v1";
  const keyStep = cloudKey
    ? invoke("routing_set_secret", { apiKeyRef: "openrouter", secret: cloudKey })
    : Promise.resolve();
  keyStep.then(() => invoke("routing_ensure_openrouter"))
    .then(() => invoke("routing_get"))
    .then((routing) => {
      let cloud = (routing.backends || []).find((b) => b.id === "openrouter");
      if (!cloud) {
        cloud = { id: "openrouter", kind: "openai", base_url: url, label: "OpenRouter", api_key_ref: "openrouter" };
        routing.backends.push(cloud);
      } else {
        cloud.base_url = url;
      }
      return invoke("routing_save", { routing });
    })
    .then(() => invoke("routing_probe", { backendId: "openrouter" }))
    .then((msg) => {
      const node = $("route-probe-cloud-status");
      node.textContent = msg;
      node.className = "models-inline-status ok";
      return refreshCloudKeyStatus().then(() => loadRouteModels("openrouter"));
    })
    .catch((error) => {
      const node = $("route-probe-cloud-status");
      node.textContent = String(error);
      node.className = "models-inline-status bad";
    });
};

(function buildEmpty() {
  const host = $("empty");
  if (window.HelixMobiusReact?.welcome) {
    reactWelcome = window.HelixMobiusReact.welcome(host, {
      starter: useStarter,
      action: (action) => {
        if (action === "web") openWeb();
        else if (action === "files") pickAttachments();
        else if (action === "picture") pickPictures();
        else if (action === "image") openCreateImage();
        else if (action === "voice") dictateButton.click();
        else if (action === "analysis") $("open-analysis").click();
        else if (action === "brief") openChatContext();
        else if (action === "tools") {
          reactTools?.open(view);
          useStarter("Research this with public sources. Cite pages in the answer.");
        }
        else if (action === "models") $("open-models").click();
      },
    });
    drawSuggestions();
    return;
  }
  host.append(el("div", "empty-kicker", "Mobius"));
  host.append(el("div", "empty-title", "How can I help?"));
  host.append(el("div", "empty-note", "Ask anything, or type / for commands. Press Ctrl+Shift+A to have Mobius act on the desktop."));
  const suggestions = el("div", "suggestions");
  host.append(suggestions);
  drawSuggestions();
})();

function queueReport() {
  if (!reporting) return;
  clearTimeout(reportTimer);
  reportTimer = setTimeout(sendReport, 120);
}
const layoutErrors = [];
window.addEventListener("error", (event) => {
  if (!reporting) return;
  layoutErrors.push(String(event.message).slice(0, 1000));
  queueReport();
});
window.addEventListener("unhandledrejection", (event) => {
  if (!reporting) return;
  layoutErrors.push(String(event.reason).slice(0, 1000));
  queueReport();
});
function sendReport() {
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((node) => {
    const box = node.getBoundingClientRect();
    if (box.width > 0 && box.height > 0 && getComputedStyle(node).visibility !== "hidden") elements[node.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
  });
  invoke("report_layout", { report: { width: window.innerWidth, height: window.innerHeight, elements, errors: layoutErrors.slice(-10) } });
}
window.addEventListener("resize", queueReport);
new MutationObserver(queueReport).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ["open", "hidden"] });

invoke("layout_reporting").then((on) => { reporting = on; if (on) queueReport(); });
invoke("chat_status").then((s) => { status = s; drawChat(); });
invoke("chat_current_route").then((route) => {
  currentModel = (route && route.model) || "";
  currentBackend = (route && route.backend) || "local";
  drawComposerMeta();
}).catch(() => {});
refreshModelTitles();
listen("library-changed", () => { refreshModelTitles(); });
invoke("personas_get").then(drawPersonas);
invoke("chat_view").then((v) => { view = v; drawChat(); invoke("chat_list").then(drawList); });
input.focus();
syncSendEnabled();

function pollBarHandoff() {
  invoke("chat_take_bar_handoff").then((changed) => {
    if (!changed) return;
    invoke("chat_view").then((v) => { view = v; drawChat(); });
    invoke("chat_list").then(drawList);
  }).catch(() => {});
}
window.addEventListener("focus", pollBarHandoff);
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible") pollBarHandoff();
});
setInterval(pollBarHandoff, 2500);
refreshAgentDeskGlance();
setInterval(refreshAgentDeskGlance, 5000);
window.addEventListener("focus", refreshAgentDeskGlance);

const reactTools = window.HelixMobiusReact?.tools($("tools-host"));
$("meta-tools").onclick = () => reactTools?.open(view);
$("meta-agent-desk").onclick = () => invoke("agent_desk_focus").then(refreshAgentDeskGlance).catch(report);
