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
let composerAct = false;
let composerPlan = false;
let modeChat = "";
let view = { id: "", title: "New chat", messages: [], streaming: false, error: "", temporary: false, pinned: false, project: "" };
let lastWaitNotice = "";
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
let routeKey = "";
function refreshRoute() {
  return invoke("chat_current_route")
    .then((route) => {
      currentModel = (route && route.model) || "";
      currentBackend = (route && route.backend) || "local";
      drawComposerMeta();
    })
    .catch(() => undefined);
}
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
  { cmd: "/settings", desc: "Settings: models, providers, generation", run: () => openRouting() },
  { cmd: "/models", desc: "Settings: choose and get models", run: () => openRouting("models") },
  { cmd: "/route", desc: "Settings: where each job runs", run: () => openRouting("models") },
  { cmd: "/attach", desc: "Attach text / source files", run: () => pickAttachments() },
  { cmd: "/web", desc: "Pick online sources yourself", run: (text) => { openWeb(text.replace(/^\/web\s*/i, "")); return Promise.resolve(); } },
  { cmd: "/remember", desc: "Remember something in every chat (local models only)", run: (typed) => {
    const fact = (typed || "").replace(/^\/remember\b/i, "").trim();
    if (!fact) { openMemory(); return Promise.resolve(); }
    return invoke("memory_add", { text: fact }).then(() => {
      const banner = $("banner");
      banner.textContent = "Remembered. /memory shows everything Mobius remembers.";
      banner.hidden = false;
      setTimeout(() => { if (banner.textContent.startsWith("Remembered")) banner.hidden = true; }, 4000);
    });
  } },
  { cmd: "/memory", desc: "See and edit what Mobius remembers", run: () => { openMemory(); return Promise.resolve(); } },
  { cmd: "/side", desc: "A side chat: ask something that reads this chat without adding to it", run: () => startSideChat().catch(report) },
  { cmd: "/tools", desc: "Configure Tools mode (research + project commands)", run: () => { openTools(); return Promise.resolve(); } },
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
  ? window.HelixMobiusReact.slash($("slash-menu"), (index) => { if (mentionRows.length) pickMention(mentionRows[index]); else if (slashShown[index]) runSlash(slashShown[index]); })
  : null;

let knowledgePeek = [];
let knowledgePeekTimer;
let knowledgeInChat = true;
invoke("knowledge_status").then((s) => { if (s) knowledgeInChat = !!s.use_in_chat; }).catch(() => {});
listen("knowledge-changed", (event) => { knowledgeInChat = !!event.payload?.use_in_chat; knowledgePeek = []; drawKnowledgeChips(); });
function drawKnowledgeChips() {
  const host = $("knowledge-chips");
  if (!knowledgePeek.length || composerMode() !== "ask" || view.tools_enabled || view.analysis_job || !view.id) {
    host.hidden = true;
    host.replaceChildren();
    return;
  }
  host.hidden = false;
  host.replaceChildren();
  const lead = el("span", "knowledge-lead", "From your files");
  lead.dataset.test = "knowledge-lead";
  host.append(lead);
  knowledgePeek.forEach((hit, i) => {
    const chip = el("span", "knowledge-chip");
    chip.dataset.test = `knowledge-chip-${i}`;
    chip.title = hit.excerpt || "";
    chip.textContent = hit.heading ? `${hit.title} › ${hit.heading}` : hit.title;
    host.append(chip);
  });
}
function scheduleKnowledgePeek() {
  clearTimeout(knowledgePeekTimer);
  const query = input.value.trim();
  const localBackend = !status.backend || status.backend === "On this computer" || String(status.backend).toLowerCase().startsWith("local");
  if (query.length < 8 || composerMode() !== "ask" || view.tools_enabled || view.analysis_job || !view.id || !localBackend || !knowledgeInChat) {
    knowledgePeek = [];
    drawKnowledgeChips();
    return;
  }
  knowledgePeekTimer = setTimeout(() => {
    invoke("knowledge_peek", { query }).then((hits) => { knowledgePeek = Array.isArray(hits) ? hits : []; drawKnowledgeChips(); }).catch(() => { knowledgePeek = []; drawKnowledgeChips(); });
  }, 450);
}

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

// The window the person was using when they summoned Mobius (the compositor notes it at the key press). A chip they can remove; sent with the
// next message as a small note, and then forgotten.
let summonNote = null;
function drawSummonChip() {
  const chip = $("summon-chip");
  chip.hidden = !summonNote;
  if (!summonNote) return;
  const what = summonNote.title || summonNote.app_id || "a window";
  chip.textContent = `About: ${what.length > 48 ? what.slice(0, 47) + "…" : what} · sent with your message ×`;
}
// The text they had selected there, read once when they summon (never otherwise): a second chip, removable, sent only if left.
let summonSelection = "";
function drawSelectionChip() {
  const chip = $("summon-selection-chip");
  chip.hidden = !summonSelection;
  if (!summonSelection) return;
  const first = summonSelection.split("\n")[0];
  chip.textContent = `Selected: “${first.length > 40 ? first.slice(0, 39) + "…" : first}” · ${summonSelection.length} characters · sent with your message ×`;
}
$("summon-selection-chip").onclick = () => { summonSelection = ""; drawSelectionChip(); };
async function checkSummon() {
  try {
    const note = await invoke("summon_context");
    if (note && (note.title || note.app_id)) {
      summonNote = note; drawSummonChip(); input.focus();
      invoke("summon_selection").then((text) => { summonSelection = text || ""; drawSelectionChip(); }, () => undefined);
    }
  } catch { /* no compositor to ask */ }
}
$("summon-chip").onclick = () => { summonNote = null; drawSummonChip(); };
window.addEventListener("focus", () => { checkSummon(); });
setTimeout(checkSummon, 600);

function attachmentPayload() {
  const files = pendingAttachments.map((a) => ({ name: a.name, content: a.content, original_chars: a.original_chars }));
  if (summonNote) {
    const lines = [`The person summoned you from another window. Title: ${summonNote.title || "(none)"}`, `Application: ${summonNote.app_id || "(unknown)"}`];
    files.unshift({ name: "the window they were using", content: lines.join("\n"), original_chars: lines.join("\n").length });
    summonNote = null; drawSummonChip();
  }
  if (summonSelection) {
    files.unshift({ name: "the text they had selected", content: summonSelection, original_chars: summonSelection.length });
    summonSelection = ""; drawSelectionChip();
  }
  if (stageOpen && stageAttach && stageOpen.text.trim()) {
    const text = stageOpen.text;
    const how = "\n\n[To change this document, reply with a ```docops block holding a JSON list (at most 10) of edits, each one of: {\"op\":\"replace\",\"find\":\"exact old text\",\"with\":\"new text\"}, {\"op\":\"delete\",\"find\":\"...\"}, {\"op\":\"insert_after\",\"anchor\":\"exact text on a line\",\"text\":\"...\"}, {\"op\":\"replace_section\",\"heading\":\"Heading text\",\"text\":\"...\"}, {\"op\":\"append\",\"text\":\"...\"}. Use this for small changes; for a full rewrite use an artifact block.]";
    files.unshift({ name: `${stageOpen.title} (the open document)`, content: Array.from(text).slice(0, 6000).join("") + how, original_chars: Array.from(text).length });
  }
  return files;
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

// Text the person chose on a piece of ephemeral UI goes as an ordinary message (queued if a reply is still being written).
function sendPlain(text) {
  if (view.streaming) { if (queue.length < MAX_QUEUE) { queue.push(text); drawQueue(); } return; }
  stickToBottom = true;
  invoke("chat_send", { text, attachments: null }).catch(report);
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


// `@` in the composer: a word starting with @ before the caret offers files from the chat's project folder and other chats. Choosing one
// attaches it as a chip (the same chips as a picked file), so what the model sees stays visible and removable.
let mentionRows = [];
let mentionToken = null;
let mentionSeq = 0;
function mentionAtCaret() {
  const caret = input.selectionStart ?? input.value.length;
  const before = input.value.slice(0, caret);
  const found = /(^|\s)@([^\s@]*)$/.exec(before);
  return found ? { start: caret - found[2].length - 1, end: caret, query: found[2] } : null;
}
function hideMentions() {
  if (!mentionRows.length && !mentionToken) return;
  mentionRows = [];
  mentionToken = null;
  hideSlashMenu();
}
function drawMentions() {
  slashIndex = Math.max(0, Math.min(slashIndex, mentionRows.length - 1));
  slashShown = [];
  if (reactSlash) reactSlash.set(mentionRows.map((row) => ({ cmd: (row.kind === "chat" ? "Chat · " : "") + row.title, desc: row.detail })), slashIndex);
  $("slash-menu").hidden = !mentionRows.length;
}
function maybeMentionFromInput() {
  const token = mentionAtCaret();
  if (!token || input.value.startsWith("/")) { hideMentions(); return false; }
  mentionToken = token;
  const seq = ++mentionSeq;
  invoke("mentions_search", { query: token.query }).then((rows) => {
    if (seq !== mentionSeq || !mentionToken) return;
    mentionRows = rows;
    if (!rows.length) { $("slash-menu").hidden = true; return; }
    slashIndex = 0;
    drawMentions();
  }).catch(() => hideMentions());
  return true;
}
function pickMention(row) {
  const token = mentionToken || mentionAtCaret();
  hideMentions();
  if (token) {
    input.value = (input.value.slice(0, token.start) + input.value.slice(token.end)).replace(/ {2,}/g, " ");
    input.setSelectionRange(token.start, token.start);
    grow(); syncSendEnabled();
  }
  invoke("mentions_resolve", { kind: row.kind, key: row.key }).then((file) => {
    if (pendingAttachments.some((a) => a.path === file.path)) return;
    if (pendingAttachments.length >= attachmentMaxFiles) { report(`A message can include up to ${attachmentMaxFiles} files.`); return; }
    pendingAttachments.push(file);
    drawAttachChips();
    syncSendEnabled();
    input.focus();
  }).catch(report);
}


// A message that reads like a desktop command ("turn the volume down") gets a one-click offer to send it with Act. A small embedding model
// in this process decides (decide_message); without the model installed nothing is offered.
let hintTimer = 0;
let hintSeq = 0;
function hideActHint() { $("act-hint").hidden = true; hintSeq++; clearTimeout(hintTimer); }
function scheduleActHint() {
  hideActHint();
  const typed = input.value.trim();
  if (composerMode() !== "ask" || view.streaming || typed.length < 8 || typed.startsWith("/") || /(^|\s)@\S*$/.test(typed)) return;
  const seq = hintSeq;
  hintTimer = setTimeout(() => {
    invoke("decide_message", { text: typed }).then((decision) => {
      if (seq !== hintSeq || input.value.trim() !== typed || composerMode() !== "ask") return;
      $("act-hint").hidden = !(decision && decision.route === "act");
    }).catch(() => undefined);
  }, 500);
}
$("act-hint").onclick = () => { hideActHint(); setComposerMode("act"); };

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
  const mark = el("span", "typing-dots");
  mark.dataset.test = "typing";
  mark.setAttribute("aria-label", "Writing");
  mark.append(el("span", "dot"), document.createTextNode("Writing"));
  return mark;
}
function writingCaret() {
  const caret = el("span", "writing-caret");
  caret.dataset.test = "writing-caret";
  caret.setAttribute("aria-hidden", "true");
  return caret;
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
  else if (last && view.streaming) body.append(writingCaret());
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
  const shown = !anchor.hidden && anchor.getBoundingClientRect().width > 0;
  const box = (shown ? anchor : $("meta-more")).getBoundingClientRect();
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
  if (!view.workspace) return "Folder";
  const parts = view.workspace.split("/").filter(Boolean);
  const name = parts.length ? parts[parts.length - 1] : view.workspace;
  return name;
}

// What each workspace asks of the chat (docs/design/mobius-workspaces.md, "The chat shapes itself to the mode"): the composer's wording and the
// starters shown while the conversation is empty. The model is told the stage's edit format in the starter itself, so a small change comes back as
// edits the person applies, not a rewrite.
const IMAGE_FORMAT = " If it needs edits, reply with one ```imageops block: a JSON list (at most 10) of {\"op\":\"crop\",\"x\":0,\"y\":0,\"w\":100,\"h\":100}, {\"op\":\"resize\",\"percent\":50}, {\"op\":\"rotate\",\"degrees\":90}, {\"op\":\"flip\",\"axis\":\"horizontal\"}, {\"op\":\"brightness\",\"amount\":20}, {\"op\":\"contrast\",\"amount\":20}, {\"op\":\"grayscale\"}, {\"op\":\"invert\"}, {\"op\":\"blur\",\"sigma\":3}, {\"op\":\"sharpen\",\"amount\":2}.";
const WORKSPACE_UI = {
  write: {
    placeholder: "Ask, or say what to change",
    title: "Write with Mobius",
    note: "Mobius reads the open document. Small changes come back as edits you apply.",
    starters: [
      ["Make it shorter", "Make the document shorter and keep its meaning."],
      ["Fix spelling and grammar", "Fix spelling and grammar. Change as little else as you can."],
      ["Continue writing", "Continue the document with the next section."],
      ["Suggest an outline", "Suggest an outline for this document."],
    ],
  },
  image: {
    placeholder: "Describe a change to this picture",
    title: "Edit a picture with Mobius",
    note: "Open a picture, then ask. Suggested edits wait for you to apply them.",
    starters: [
      ["Describe this picture", "Describe this picture."],
      ["Brighter, more contrast", "Make the picture a little brighter with more contrast." + IMAGE_FORMAT],
      ["Black and white", "Make the picture black and white." + IMAGE_FORMAT],
      ["Crop to a square", "Crop the picture to a centred square." + IMAGE_FORMAT],
    ],
  },
  code: {
    placeholder: "Ask about the project, or give a task",
    title: "Work on code with Mobius",
    note: "Each step is shown, and every change waits for your review.",
    starters: [
      ["Explain this project", "Explain what this project is and how it is organised."],
      ["Find likely bugs", "Read the main files and point out likely bugs."],
      ["Add tests", "Suggest tests this project is missing, and write the first one."],
      ["Summarise recent changes", "Summarise what changed recently in this project."],
    ],
  },
};
function drawModeStarters() {
  const host = $("mode-starters"), ui = WORKSPACE_UI[workspace];
  const empty = view.messages.length === 0 && (view.tasks || []).length === 0;
  $("empty").hidden = !empty || !!ui;
  host.hidden = !(empty && ui);
  host.replaceChildren();
  drawDockActions();
  if (host.hidden) return;
  host.append(el("h3", "", ui.title), el("p", "", ui.note));
  const row = el("div", "mode-starter-row");
  for (const [label, prompt] of ui.starters) {
    const b = el("button", "mode-starter", label);
    b.type = "button"; b.dataset.test = "starter-" + starterId(label);
    b.onclick = () => sendPlain(prompt);
    row.append(b);
  }
  host.append(row);
}

function starterId(label) {
  return label.toLowerCase().replace(/[^a-z]+/g, "-").replace(/-$/, "");
}

// Once a work mode has a conversation, the same actions stay above the composer. The empty screen keeps the larger starters.
function drawDockActions() {
  const host = $("dock-actions");
  if (!host) return;
  const ui = WORKSPACE_UI[workspace];
  const empty = !view || (view.messages.length === 0 && (view.tasks || []).length === 0);
  host.hidden = !ui || empty || !!view.analysis_job;
  host.replaceChildren();
  if (host.hidden) return;
  for (const [label, prompt] of ui.starters) {
    const button = el("button", "dock-action", label);
    button.type = "button";
    button.dataset.test = "dock-" + starterId(label);
    button.onclick = () => sendPlain(prompt);
    host.append(button);
  }
}

function composerPlaceholder() {
  if (listening || dictationBusy === "transcribing") return;
  if (view.analysis_job) {
    input.placeholder = "Ask about the local analysis evidence";
    return;
  }
  input.placeholder = composerAct
    ? "Say what to do on the desktop"
    : composerPlan
    ? "Describe the task — Mobius will write a plan, and nothing runs yet"
    : WORKSPACE_UI[workspace] && !view.tools_enabled
    ? WORKSPACE_UI[workspace].placeholder
    : view.tools_enabled
    ? "Give Agent a task — each step is shown, then it continues"
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
  if (modeChat !== view.id) { if (modeChat) { composerAct = false; composerPlan = false; } modeChat = view.id; }
  const mode = composerMode();
  $("mode").textContent = mode === "act" ? "Desktop" : mode === "agent" ? "Agent" : mode === "plan" ? "Plan" : "Ask";
  $("mode").className = "mode-chip" + (mode === "ask" ? "" : " mode-" + (mode === "agent" ? "tools" : mode));
  $("mode").title = "Mode: " + $("mode").textContent + " (Shift+Tab to change)";
  const build = $("plan-build");
  if (build) build.hidden = mode !== "plan" || !!view.streaming || !(view.messages || []).some((m) => m.role === "assistant");
  $("meta-tools").hidden = !view.tools_enabled;
  $("meta-tools").textContent = toolsChipLabel();
  $("meta-tools").classList.toggle("tools-on", !!view.tools_enabled);
  $("meta-tools").disabled = !view.id || view.streaming || !!view.analysis_job;
  $("meta-tools").title = view.analysis_job
    ? "Tools mode is unavailable during software analysis"
    : view.tools_enabled
      ? "Tools mode is on for this chat — click to change folder or turn off"
      : "Turn on Tools mode for in-chat research and reviewed project commands";
  const context = view.context;
  const ctxBtn = $("meta-context");
  const ctxLabel = ctxBtn.querySelector(".context-label");
  const ring = ctxBtn.querySelector(".context-ring");
  const ctxText = context?.omitted_messages ? "Recent only" : (view.brief ? "Brief set" : "Context");
  if (ctxLabel) ctxLabel.textContent = ctxText;
  else ctxBtn.textContent = ctxText;
  ctxBtn.classList.toggle("context-limited", !!context?.omitted_messages);
  ctxBtn.disabled = !view.id;
  if (ring && context?.budget_chars) {
    const modelChars = context.model_context_tokens ? context.model_context_tokens * 4 : 0;
    const fill = modelChars
      ? Math.min(1, context.used_chars / modelChars)
      : Math.min(1, context.used_chars / context.budget_chars);
    ring.style.setProperty("--context-fill", String(fill));
    ctxBtn.title = modelChars
      ? `Chat context: about ${context.used_chars.toLocaleString()} characters of a ${context.model_context_tokens.toLocaleString()}-token model window (recent-history budget ${context.budget_chars.toLocaleString()} chars). Click for the brief.`
      : `Chat context: ${context.used_chars.toLocaleString()} of ${context.budget_chars.toLocaleString()} characters of recent history. Click for the brief.`;
  } else if (ring) ring.style.removeProperty("--context-fill");
  drawQueue();
  drawProjectFilter();
  updateJumpLatest();
  composerPlaceholder();
  drawKnowledgeChips();
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

// The composer moves between the middle of the window (an empty chat) and the bottom (a conversation) with a short, decelerating glide, so the
// change of layout is something that happened rather than something that flickered. Skipped for people who ask for less motion.
function setHome(on) {
  if (document.body.classList.contains("is-home") === on) return;
  const wrap = document.querySelector(".composer-wrap");
  const before = wrap ? wrap.getBoundingClientRect() : null;
  document.body.classList.toggle("is-home", on);
  if (!wrap || !before || window.matchMedia("(prefers-reduced-motion: reduce)").matches || !wrap.animate) return;
  const after = wrap.getBoundingClientRect();
  const dy = before.top - after.top;
  if (Math.abs(dy) < 4) return;
  const glide = wrap.animate([{ transform: `translateY(${dy}px)` }, { transform: "none" }], { duration: 340, easing: "cubic-bezier(.2, .7, .2, 1)" });
  // The layout report (the tests read it) must describe where the composer ended up, not where it was part-way.
  glide.finished.then(() => queueReport(), () => undefined);
}

function waitingStep() {
  for (let i = view.messages.length - 1; i >= 0; i--) {
    if (view.messages[i].role !== "assistant") continue;
    return (view.messages[i].activities || []).find((a) => a.state === "waiting") || null;
  }
  return null;
}
function listLive() {
  const wait = waitingStep();
  return { streaming: !!view.streaming && !wait, waiting: !!wait };
}
function chatSurfaceVisible() {
  return workspace === "chat" && !view.analysis_job && !document.hidden && document.hasFocus();
}
function drawApprovalHint() {
  const wait = waitingStep();
  const needs = $("needs-you");
  if (wait && !chatSurfaceVisible()) {
    needs.hidden = false;
    const note = waitingNotice(wait);
    needs.textContent = `${note.title} — show the chat`;
    needs.onclick = () => { setWorkspace("chat"); closeSideDrawer(); scrollDown(); document.querySelector(".tool-activity.waiting [data-test=tool-approve]")?.focus(); };
  } else needs.hidden = true;
  if (wait && wait.id !== lastWaitNotice && !chatSurfaceVisible()) {
    const note = waitingNotice(wait);
    invoke("mobius_notify", { summary: note.title, body: note.body }).catch(() => {});
    lastWaitNotice = wait.id;
  }
  if (!wait) lastWaitNotice = "";
}
function waitingNotice(step) {
  const line = (step.input || "").split("\n")[0].trim();
  if (step.tool === "terminal") return { title: "Approve a command", body: line || "A terminal command is waiting." };
  if (step.tool === "edit_file") return { title: "Review a file edit", body: line || "A file edit is waiting." };
  if (step.tool === "agent_permission") return { title: "Allow the agent", body: line || "The agent is waiting for permission." };
  return { title: "Mobius needs you", body: line || "A tool step is waiting for your review." };
}
function agentStatusLabel() {
  const wait = waitingStep();
  if (wait) {
    if (wait.tool === "terminal") return "Approve command";
    if (wait.tool === "edit_file") return "Review edit";
    if (wait.tool === "web_read") return "Approve page";
    return "Needs you";
  }
  let running = null;
  for (let i = view.messages.length - 1; i >= 0; i--) {
    if (view.messages[i].role !== "assistant") continue;
    const acts = view.messages[i].activities || [];
    running = acts.findLast ? acts.findLast((a) => a.state === "running") : [...acts].reverse().find((a) => a.state === "running");
    break;
  }
  if (!running) return view.tools_enabled ? "Agent working" : "Writing";
  if (running.tool === "web_search") return "Searching";
  if (running.tool === "web_read") return "Reading a page";
  if (running.tool === "terminal") return "Running a command";
  if (running.tool === "edit_file") return "Editing a file";
  if (running.tool === "answer") return "Writing";
  return view.tools_enabled ? "Agent working" : "Writing";
}
function drawAgentStatus() {
  const chip = $("agent-status");
  if (!chip) return;
  const wait = waitingStep();
  if (!view.streaming && !wait) { chip.hidden = true; chip.onclick = null; return; }
  chip.hidden = false;
  chip.textContent = agentStatusLabel();
  chip.classList.toggle("waiting", !!wait);
  chip.title = wait ? "Show the step that needs you" : "What Mobius is doing";
  chip.onclick = wait
    ? () => document.querySelector(".tool-activity.waiting [data-test=tool-approve]")?.focus()
    : null;
}
function drawChat() {
  $("analysis-evidence").hidden = !view.analysis_job;
  if (!view.streaming) $("loading-chip").hidden = true;
  drawAgentStatus();
  $("chat-title").textContent = view.temporary ? "Temporary chat" : (view.title || "New chat");
  drawComposerMeta();
  drawKnowledgeChips();
  if (reactThread) {
    reactThread.set(view.messages, view.streaming, view.context, view.tasks || []);
  } else {
    const host = $("messages");
    host.replaceChildren();
    view.messages.forEach((message, index) => host.append(messageNode(message, index, index === view.messages.length - 1)));
  }
  const emptyChat = view.messages.length === 0 && (view.tasks || []).length === 0;
  $("empty").hidden = !emptyChat;
  drawModeStarters();
  if (workspace === "code") reactCode?.update();
  // An empty chat is a home: the greeting and the composer sit in the middle of the window, and the composer settles to the bottom with the first message.
  setHome(emptyChat && workspace === "chat");
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
  drawApprovalHint();
  drawList();
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
  // Another chat, or this chat's own model changed: the chip shows the model the next message will use.
  const key = `${view.id}|${view.route_backend || ""}|${view.route_model || ""}`;
  if (key !== routeKey) { routeKey = key; refreshRoute(); }
  drawChat();
  if (pendingProject && view.id && view.messages.length && !view.temporary) { const joined = pendingProject; pendingProject = ""; invoke("chat_set_project", { project: joined }).catch(report); }
  if (sideOf && !view.temporary) sideOf = null;
  drawSideChip();
  if (workspaceChat !== view.id) { if (workspaceChat) { setWorkspace("chat"); } workspaceChat = view.id; }
  if (wasStreaming && !view.streaming) { recordArtifacts(); reactCode?.refresh(); flushQueue(); }
});
listen("chat-stream", (event) => {
  // A streamed update that arrives after the reply finished is stale; showing it would replace the finished reply with an earlier cut of it.
  if (!view.streaming) return;
  if (reactThread) {
    reactThread.stream(event.payload.blocks);
    if (stickToBottom) scrollDown();
    return;
  }
  const last = $("messages").lastElementChild;
  if (!last || !last.classList.contains("assistant")) return;
  const body = last.querySelector(".msg-body");
  drawBlocks(body, event.payload.blocks);
  body.append(event.payload.blocks?.length ? writingCaret() : typingDots());
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
    stage: (title, body) => reactStage?.open(title, body),
    editPicture: (id) => setWorkspace("image", id),
    applyDocEdits: (ops) => {
      if (!stageOpen) { report("Open the document on the Write stage first."); return; }
      reactStage?.propose(ops);
    },
    applyEdits: (ops) => { if (workspace !== "image") report("Open the picture on the Image stage first (Edit on a picture, or the Image place)."); else reactImage?.apply(ops); },
    quote: (text) => {
      const prefix = input.value && !input.value.endsWith("\n") ? "\n\n" : "";
      input.value = input.value + prefix + text;
      grow();
      syncSendEnabled();
      input.focus();
    },
    edited: (index, text) => {
      stickToBottom = true;
      invoke("chat_edit", { index, text })
        .then(() => invoke("chat_status").then((s) => { status = s; drawComposerMeta(); }))
        .catch(report);
    },
    fork: (index) => forkChat(index),
    send: (text) => sendPlain(text),
    report: (error) => report(error),
    undo: (id) => invoke("chat_act_undo", { task: id })
      .then(() => { if (reactThread) reactThread.undo(id, false); })
      .catch((error) => {
        if (reactThread) reactThread.undo(id, false);
        report(error);
      }),
    openProjectFile: (path) => openProjectFile(path),
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
  reactWelcome?.recents(chatItems.filter((c) => !c.temporary).slice(0, 3).map((c) => ({ id: c.id, title: c.title, project: c.project })));
  if (reactChats) { reactChats.set(chatItems, view.id, listLive()); return; }
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
    const line = el("span", "chat-open-line");
    const live = item.id === view.id ? listLive() : null;
    if (live?.waiting) {
      const mark = el("span", "chat-waiting");
      mark.dataset.test = "chat-waiting";
      mark.title = "Needs your review";
      line.append(mark);
    } else if (live?.streaming) {
      const working = el("span", "chat-working");
      working.dataset.test = "chat-working";
      working.title = "Writing";
      line.append(working);
    }
    line.append(el("span", "chat-open-title", item.title));
    open.append(line);
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
// Renaming a chat happens in the header, where the title is: click it, type, Enter. Escape puts it back.
function startInlineRename() {
  const button = $("chat-title");
  if (document.querySelector(".chat-title-input")) return;
  const field = document.createElement("input");
  field.className = "chat-title-input";
  field.dataset.test = "chat-title-input";
  field.value = view.title || "New chat";
  field.maxLength = 80;
  field.setAttribute("aria-label", "Chat title");
  button.hidden = true;
  button.after(field);
  field.focus();
  field.select();
  let finished = false;
  const finish = (commit) => {
    if (finished) return;
    finished = true;
    const title = field.value.trim();
    field.remove();
    button.hidden = false;
    if (commit && title && title !== view.title) invoke("chat_rename", { title }).catch(report);
  };
  field.onkeydown = (event) => {
    if (event.key === "Enter") { event.preventDefault(); finish(true); input.focus(); }
    else if (event.key === "Escape") { event.preventDefault(); finish(false); input.focus(); }
  };
  field.onblur = () => finish(true);
}
$("chat-title").onclick = () => {
  if (view.temporary) return;
  if (view.id) { startInlineRename(); return; }
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
  if (!maybeSlashFromInput()) maybeMentionFromInput();
  scheduleActHint();
  scheduleKnowledgePeek();
  if (promptHistoryIndex >= 0 && input.value !== promptHistory[promptHistoryIndex]) {
    promptHistoryIndex = -1;
    promptDraft = "";
  }
});
input.addEventListener("keydown", (event) => {
  if (event.key === "Tab" && event.shiftKey && !event.isComposing) {
    event.preventDefault();
    const order = ["agent", "ask", "plan", "act"];
    setComposerMode(order[(order.indexOf(composerMode()) + 1) % order.length]);
    return;
  }
  if (mentionRows.length && !$("slash-menu").hidden) {
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      slashIndex = (slashIndex + (event.key === "ArrowDown" ? 1 : mentionRows.length - 1)) % mentionRows.length;
      drawMentions();
      return;
    }
    if (event.key === "Enter" || event.key === "Tab") { event.preventDefault(); pickMention(mentionRows[slashIndex]); return; }
    if (event.key === "Escape") { event.preventDefault(); hideMentions(); return; }
  }
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
  hideActHint();
  hideSlashMenu();
  const text = input.value;
  if (!text.trim() && !pendingAttachments.length && !pendingPictures.length) return;
  if (text.trim().startsWith("/") && tryRunSlashSubmit()) return;
  if (composerAct) { if (text.trim()) actFromComposer(text); return; }
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
  invoke("chat_send", { text, attachments: attachments.length ? attachments : null, pictures: pictures.length ? pictures : null, plan: composerMode() === "plan" })
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
// Harness modes, as in other agents: Agent does the work, Ask only answers, Plan writes a plan, Desktop is one desktop message.
function composerMode() { return composerAct ? "act" : composerPlan ? "plan" : view.tools_enabled ? "agent" : "ask"; }
function setComposerMode(next) {
  hideActHint();
  if (next === "tools") next = "agent";
  if (next === "desktop") next = "act";
  if (next === "folder") { openTools(); return; }
  if (next === "act") { composerAct = true; composerPlan = false; drawComposerMeta(); input.focus(); return Promise.resolve(); }
  composerAct = false;
  composerPlan = next === "plan";
  drawComposerMeta();
  const enabled = next === "agent";
  const pending = enabled !== !!view.tools_enabled
    ? invoke("tools_configure", { chatId: view.id, enabled, workspace: view.workspace || "" }).catch(report)
    : Promise.resolve();
  input.focus();
  return pending;
}
$("plan-build").onclick = () => {
  setComposerMode("agent").then(() => {
    invoke("chat_send", { text: "Build the plan above.", attachments: null, pictures: null, plan: false }).catch(report);
  });
};
window.addEventListener("mobius-set-mode", (event) => setComposerMode(event.detail));
function modeState() {
  return { mode: composerMode(), folder: view.workspace || "", busy: !view.id || view.streaming || !!view.analysis_job };
}
$("mode").onclick = () => {
  const box = $("mode").getBoundingClientRect();
  if (reactDialogs) reactDialogs.openMenu("mode", { left: box.left, top: box.top, width: box.width }, modeState());
};
function actFromComposer(text) {
  const typed = text.trim();
  // Handed over like a sent message: the draft goes, unless it was changed meanwhile or the hand-over failed.
  composerAct = false;
  drawComposerMeta();
  return actOnDesktop(typed, () => {
    if (input.value.trim() === typed) { input.value = ""; grow(); syncSendEnabled(); }
  });
}
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

// The composer's footer holds the model, Tools when on, and a "⋯" that opens the rest (persona, style, where it runs, context).
function openMoreMenu() {
  document.querySelectorAll(".more-menu").forEach((m) => m.remove());
  const box = $("meta-more").getBoundingClientRect();
  const menu = el("div", "hx-menu more-menu");
  menu.dataset.test = "menu-more";
  menu.style.left = Math.max(8, Math.min(box.left, window.innerWidth - 300)) + "px";
  menu.style.bottom = (window.innerHeight - box.top + 6) + "px";
  menu.style.minWidth = "260px";
  const add = (id, title, sub, chip) => {
    const b = el("button", "hx-menu-item");
    b.type = "button";
    b.dataset.test = "menu-more-" + id;
    const text = el("span", "mi-text");
    text.append(el("span", "mi-title", title));
    if (sub) text.append(el("span", "mi-sub", sub));
    b.append(text);
    b.onclick = () => { menu.remove(); chip.click(); };
    menu.append(b);
  };
  if (!$("meta-persona").hidden) add("persona", "Persona", $("meta-persona").textContent, $("meta-persona"));
  if (!$("meta-style").hidden) add("style", "Style", $("meta-style").textContent.replace(/^Style\s*·?\s*/, "") || "None", $("meta-style"));
  add("route", "Where it runs", $("meta-route").textContent || "On this computer", $("meta-route"));
  add("context", "Context and brief", "What this chat keeps in mind", $("meta-context"));
  if (!$("meta-agent-desk").hidden) add("desk", "Agent desk", "", $("meta-agent-desk"));
  document.body.append(menu);
  const close = (event) => { if (!menu.contains(event.target)) { menu.remove(); window.removeEventListener("mousedown", close, true); window.removeEventListener("keydown", esc, true); } };
  const esc = (event) => { if (event.key === "Escape") { menu.remove(); window.removeEventListener("mousedown", close, true); window.removeEventListener("keydown", esc, true); } };
  setTimeout(() => { window.addEventListener("mousedown", close, true); window.addEventListener("keydown", esc, true); }, 0);
}
$("meta-more").onclick = () => openMoreMenu();
// "+" beside the composer: one place for everything that adds to a message (the microphone stays where it is wanted most, beside Send).
function openPlusMenu() {
  document.querySelectorAll(".plus-menu").forEach((m) => m.remove());
  const box = $("plus").getBoundingClientRect();
  const menu = el("div", "hx-menu plus-menu");
  menu.dataset.test = "menu-plus";
  menu.style.left = Math.max(8, box.left) + "px";
  menu.style.bottom = (window.innerHeight - box.top + 8) + "px";
  menu.style.minWidth = "250px";
  const add = (id, title, sub, hint, run) => {
    const b = el("button", "hx-menu-item");
    b.type = "button";
    b.dataset.test = "menu-plus-" + id;
    const text = el("span", "mi-text");
    text.append(el("span", "mi-title", title), el("span", "mi-sub", sub));
    b.append(text);
    if (hint) b.append(el("kbd", "", hint));
    b.onclick = () => { menu.remove(); run(); };
    menu.append(b);
  };
  add("files", "Attach files", "Text or source files", "Ctrl+U", () => $("attach").click());
  add("picture", "Add a picture", "For a model that sees", "", () => $("attach-picture").click());
  add("image", "Create an image", "Described, then made here", "", () => openCreateImage());
  add("web", "Pick online sources", "Choose pages to ask about", "", () => openWeb());
  document.body.append(menu);
  const close = (event) => { if (!menu.contains(event.target)) { menu.remove(); window.removeEventListener("mousedown", close, true); window.removeEventListener("keydown", esc, true); } };
  const esc = (event) => { if (event.key === "Escape") { menu.remove(); window.removeEventListener("mousedown", close, true); window.removeEventListener("keydown", esc, true); input.focus(); } };
  setTimeout(() => { window.addEventListener("mousedown", close, true); window.addEventListener("keydown", esc, true); }, 0);
}
$("plus").onclick = () => openPlusMenu();

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
    if (reactPalette) reactPalette.open();
    else { openSideDrawer(); $("chat-search").focus(); $("chat-search").select(); }
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
  if (meta && event.key === "\\" && document.body.classList.contains("stage-open")) {
    event.preventDefault();
    toggleLayout();
  }
  if (meta && !event.shiftKey && (event.key.toLowerCase() === "m" || event.key === ",")) {
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

// Sidebar: a drawer on a narrow window, and a rail you can fold on a wide one (remembered).
let sideCollapsed = false;
try { sideCollapsed = localStorage.getItem("mobius-side") === "collapsed"; } catch { /* not remembered */ }
function closeSideDrawer() {
  document.body.classList.remove("side-open");
  $("side-scrim").hidden = true;
  drawSideCollapsed();
}
function openSideDrawer() {
  document.body.classList.add("side-open");
  $("side-scrim").hidden = false;
  $("side-toggle").setAttribute("aria-expanded", "true");
  $("side-toggle").title = "Conversations";
  $("side-toggle").setAttribute("aria-label", "Conversations");
}
function drawSideCollapsed() {
  const wide = window.matchMedia("(min-width: 720px)").matches;
  document.body.classList.toggle("side-collapsed", sideCollapsed && wide);
  if (!wide) return;
  const open = !sideCollapsed;
  $("side-toggle").setAttribute("aria-expanded", String(open));
  $("side-toggle").title = open ? "Hide the sidebar" : "Show the sidebar";
  $("side-toggle").setAttribute("aria-label", $("side-toggle").title);
}
function toggleSideCollapsed() {
  sideCollapsed = !sideCollapsed;
  try { localStorage.setItem("mobius-side", sideCollapsed ? "collapsed" : "open"); } catch { /* not remembered */ }
  drawSideCollapsed();
}
$("side-toggle").onclick = () => {
  if (window.matchMedia("(min-width: 720px)").matches) toggleSideCollapsed();
  else if (document.body.classList.contains("side-open")) closeSideDrawer();
  else openSideDrawer();
};
$("side-scrim").onclick = () => closeSideDrawer();
window.matchMedia("(min-width: 720px)").addEventListener("change", (event) => {
  if (event.matches) closeSideDrawer();
  else drawSideCollapsed();
});
drawSideCollapsed();

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
    ? "Secret saved"
    : "No key saved yet.";
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

// The Settings page is React when the bundle is present; the hand-written page below is the fallback.
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
function openAnalysis() {
  closeHubMenu();
  closeSideDrawer();
  $("mode-analyze").classList.add("active");
  if (reactAnalysis) reactAnalysis.open();
  else report("Build the Mobius frontend to use software analysis.");
}
$("open-analysis").onclick = () => openAnalysis();
$("mode-analyze").onclick = () => openAnalysis();
window.addEventListener("mobius-analysis-closed", () => $("mode-analyze").classList.remove("active"));
$("analysis-evidence").onclick = () => reactAnalysis?.open(view.analysis_job);

function openMemory() {
  if (reactModels) openRouting("memory");
  else reactMemory?.open();
}
async function openRouting(section) {
  closeHubMenu();
  closeSideDrawer();
  if (reactModels) { reactModels.open(section); return; }
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
window.addEventListener("mobius-create-image", () => openCreateImage());
window.addEventListener("mobius-open-models", (event) => openRouting(event.detail?.section).catch(report));
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
      open: (id) => invoke("chat_open", { id }).then(() => { clearAttachments(); input.focus(); }).catch(report),
      action: (action) => {
        if (action === "web") openWeb();
        else if (action === "files") pickAttachments();
        else if (action === "picture") pickPictures();
        else if (action === "image") openCreateImage();
        else if (action === "voice") dictateButton.click();
        else if (action === "analysis") $("open-analysis").click();
        else if (action === "brief") openChatContext();
        else if (action === "write") setWorkspace("write");
        else if (action === "code") setWorkspace("code");
        else if (action === "tools") {
          openTools();
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
  host.append(el("div", "empty-note", "Ask anything, or type / for commands. Shift+Tab changes the mode (Ask, Tools, Act); Ctrl+Shift+A acts on the desktop."));
  const suggestions = el("div", "suggestions");
  host.append(suggestions);
  drawSuggestions();
})();

function queueReport() {
  if (!reporting) return;
  // Throttled, not debounced: a reply being written changes the page all the time, and a debounce would never report during it.
  if (reportTimer) return;
  reportTimer = setTimeout(() => { reportTimer = 0; sendReport(); }, 120);
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
new MutationObserver(queueReport).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ["open", "hidden", "class"] });

invoke("layout_reporting").then((on) => { reporting = on; if (on) queueReport(); });
invoke("chat_status").then((s) => { status = s; drawChat(); });
refreshRoute();
refreshModelTitles();
// The model is this chat's own: it follows the chat opened, a change from the menu, and the default routing.
window.addEventListener("mobius-route-changed", () => { refreshRoute(); });
listen("routing-view", () => { refreshRoute(); });
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
window.addEventListener("focus", () => { pollBarHandoff(); drawApprovalHint(); drawList(); });
document.addEventListener("visibilitychange", () => {
  drawApprovalHint();
  drawList();
  if (document.visibilityState === "visible") pollBarHandoff();
});
setInterval(pollBarHandoff, 2500);
refreshAgentDeskGlance();
setInterval(refreshAgentDeskGlance, 5000);
window.addEventListener("focus", refreshAgentDeskGlance);

const reactTools = window.HelixMobiusReact?.tools($("tools-host"));
const reactMemory = window.HelixMobiusReact?.memory($("memory-host"));
const reactScheduled = window.HelixMobiusReact?.scheduled($("scheduled-host"), (id) => invoke("chat_open", { id }).then(() => { clearAttachments(); input.focus(); }).catch(report));
$("mode-scheduled").onclick = () => reactScheduled?.open();
const reactAgentTasks = window.HelixMobiusReact?.agentTasks($("agent-tasks-host"));
$("mode-tasks").onclick = () => reactAgentTasks?.open();
// A chat begun from the Projects page joins its project once it has its first message (an empty chat is not kept, so it cannot be moved yet).
let pendingProject = "";
const reactProjects = window.HelixMobiusReact?.projects($("projects-host"), {
  report: (error) => report(error),
  openChat: (id, doc) => {
    if (doc) openArtifactRow({ chat_id: id, title: doc });
    else invoke("chat_open", { id }).then(() => { clearAttachments(); input.focus(); }).catch(report);
  },
  newChatIn: (project) => invoke("chat_new").then(() => { pendingProject = project; clearAttachments(); input.placeholder = `Message Mobius — new chat in ${project}`; input.focus(); }).catch(report),
});
$("mode-projects").onclick = () => reactProjects?.open();

// The command palette (Ctrl+K): chats, commands, modes and models in one box.
function paletteItems(query) {
  const words = query.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const match = (text) => words.every((w) => text.toLowerCase().includes(w));
  const rows = [];
  chatItems.filter((c) => match(c.title + " " + (c.project || ""))).slice(0, words.length ? 6 : 5).forEach((c) => rows.push({
    id: "chat:" + c.id, section: "Chats", title: c.title, sub: c.project || (c.pinned ? "Pinned" : ""), run: () => invoke("chat_open", { id: c.id }).then(() => { clearAttachments(); input.focus(); }).catch(report),
  }));
  const act = (id, title, sub, hint, run) => { if (match(title + " " + sub)) rows.push({ id, section: "Do", title, sub, hint, run }); };
  act("new", "New chat", "", "Ctrl+N", () => $("new-chat").click());
  act("temp", "Temporary chat", "Not saved", "Ctrl+Shift+N", () => $("temp-chat").click());
  act("mode-agent", "Mode: Agent", "Do the work in the project, each step reviewed", "", () => setComposerMode("agent"));
  act("mode-ask", "Mode: Ask", "Answer only", "", () => setComposerMode("ask"));
  act("mode-plan", "Mode: Plan", "Write a plan before anything runs", "", () => setComposerMode("plan"));
  act("mode-act", "Mode: Desktop", "The next message goes to the desktop assistant", "", () => setComposerMode("act"));
  act("model", "Choose model", "This chat's model", "", () => $("meta-model").click());
  act("routing", "Settings", "Models, providers and how replies are written", "Ctrl+,", () => openRouting().catch(report));
  act("ws-chat", "Workspace: Chat", "Just the conversation", "", () => setWorkspace("chat"));
  act("ws-write", "Workspace: Write", "A document beside the chat", "", () => setWorkspace("write"));
  act("ws-image", "Workspace: Image", "Edit a picture beside the chat", "", () => setWorkspace("image"));
  act("ws-code", "Workspace: Code", "Project files and changes beside the chat", "", () => setWorkspace("code"));
  act("analyze", "Analyze software", "Inspect a Linux binary with REA and Ghidra. It is not run.", "", () => openAnalysis());
  act("side", "Side chat", "Ask something that reads this chat without touching it", "/side", () => startSideChat().catch(report));
  act("artifacts", "Artifacts", "Every document your chats made", "", () => showArtifacts(true));
  act("agent-tasks", "Agent tasks", "What agents did, step by step", "", () => reactAgentTasks?.open());
  act("scheduled", "Scheduled prompts", "Run a prompt by itself at a time", "", () => reactScheduled?.open());
  act("projects", "Projects", "Instructions, folder, chats and documents together", "", () => reactProjects?.open());
  act("memory", "Memory", "What Mobius remembers", "", () => openMemory());
  act("find", "Find in this chat", "", "Ctrl+F", () => openFindBar());
  act("export", "Export this chat", "Markdown", "Ctrl+Shift+C", () => $("export-chat").click());
  act("fork", "Fork this chat", "Continue from here in a new chat", "Ctrl+Shift+F", () => forkChat());
  SLASH_COMMANDS.filter((c) => ["/image", "/web", "/attach", "/tools"].includes(c.cmd)).forEach((c) => act("slash" + c.cmd, c.cmd.slice(1)[0].toUpperCase() + c.cmd.slice(2) + "…", c.desc, c.cmd, () => { input.focus(); input.value = c.cmd + " "; grow(); syncSendEnabled(); }));
  return rows;
}

// The stage (docs/design/mobius-workspaces.md): the open document goes to the model with the next message, as a chip that can be dropped.
var stageReady = false, stageWidthFor = "";
let stageOpen = null;      // { title, text } while a document is open
let stageAttach = true;    // whether it is sent with the next message
let workspace = "chat";    // chat | write | code: how the window is laid out (docs/design/mobius-workspaces.md)
let workspaceChat = "";
// Open views are tabs, the way an editor keeps Browser and Files beside the agent. Chat stays; the others appear when opened and can be closed.
const VIEW_LABELS = { chat: "Chat", write: "Write", image: "Image", code: "Code" };
let openViews = ["chat"];
try {
  const saved = JSON.parse(sessionStorage.getItem("mobius-views") || "[]");
  if (Array.isArray(saved)) openViews = ["chat", ...saved.filter((id) => VIEW_LABELS[id] && id !== "chat")];
} catch { /* not remembered */ }
function rememberViews() {
  try { sessionStorage.setItem("mobius-views", JSON.stringify(openViews.filter((id) => id !== "chat"))); } catch { /* not remembered */ }
}
function drawViewTabs() {
  const host = $("view-tabs");
  host.replaceChildren();
  host.hidden = openViews.length < 2;
  for (const id of openViews) {
    const tab = el("div", "view-tab" + (workspace === id ? " active" : ""));
    tab.dataset.test = "view-tab-" + id;
    tab.setAttribute("role", "tab");
    tab.setAttribute("aria-selected", workspace === id ? "true" : "false");
    const label = el("button", "view-tab-label", VIEW_LABELS[id]);
    label.type = "button";
    label.onclick = () => setWorkspace(id);
    tab.append(label);
    if (id !== "chat") {
      const close = el("button", "view-tab-x", "×");
      close.type = "button";
      close.dataset.test = "view-tab-close-" + id;
      close.title = "Close " + VIEW_LABELS[id];
      close.setAttribute("aria-label", close.title);
      close.onclick = (event) => { event.stopPropagation(); closeView(id); };
      tab.append(close);
    }
    host.append(tab);
  }
}
function closeView(id) {
  openViews = openViews.filter((viewId) => viewId !== id);
  rememberViews();
  if (workspace === id) setWorkspace("chat");
  else drawViewTabs();
}
drawViewTabs();
function drawStageChip() {
  const chip = $("stage-chip");
  chip.hidden = !stageOpen;
  for (const id of ["chat", "write", "image", "code"]) $("mode-" + id).classList.toggle("active", workspace === id);
  $("workspace-btn").textContent = ({ chat: "Chat", write: "Write", image: "Image", code: "Code" })[workspace] + " ▾";
  document.body.dataset.workspace = workspace;
  document.body.classList.toggle("stage-open", !!stageOpen || workspace === "code" || workspace === "image");
  drawLayout();
  if (typeof view !== "undefined" && view && view.messages) { drawModeStarters(); composerPlaceholder(); }
  drawViewTabs();
  if (stageReady) { if (stageWidthFor !== workspace) { stageWidthFor = workspace; loadStageWidth(); } drawStageSide(); }
  if (!stageOpen) return;
  chip.textContent = stageAttach ? `${stageOpen.title} · sent with your message ×` : `${stageOpen.title} · not sent (click to send)`;
  chip.classList.toggle("off", !stageAttach);
}
$("stage-chip").onclick = () => { stageAttach = !stageAttach; drawStageChip(); };

// Layout: beside (chat and stage side by side) or focus (the stage fills the window and the chat floats over it as a small window, like an
// assistant in an editor). Remembered; only meaningful while a stage is open.
let layoutFocus = false;
try { layoutFocus = localStorage.getItem("mobius-layout") === "focus"; } catch { /* not remembered */ }
function drawLayout() {
  const open = document.body.classList.contains("stage-open");
  document.body.classList.toggle("layout-stage", open && layoutFocus);
  $("layout-btn").hidden = !open;
  $("layout-btn").textContent = layoutFocus ? "⤡" : "⤢";
  $("layout-btn").title = layoutFocus ? "Chat and work side by side (Ctrl+\\)" : "Focus the work: the chat becomes a small window over it (Ctrl+\\)";
  $("layout-btn").setAttribute("aria-label", layoutFocus ? "Show the chat beside the work" : "Focus the work");
}
function toggleLayout() {
  layoutFocus = !layoutFocus;
  try { localStorage.setItem("mobius-layout", layoutFocus ? "focus" : "beside"); } catch { /* not remembered */ }
  drawLayout();
}
$("layout-btn").onclick = () => toggleLayout();

// The work area is the person's to shape: drag the divider (or use the arrow keys on it) to set its width, double-click to reset it; the width is
// remembered per workspace. The ⇄ button puts it on the other side of the chat.
const STAGE_MIN = 300, STAGE_MIN_H = 160;
const DOCKS = [["right", "Right of the chat"], ["left", "Left of the chat"], ["bottom", "Below the chat"], ["top", "Above the chat"]];
let stageDock = "right";
try {
  const saved = localStorage.getItem("mobius-stage-dock") || localStorage.getItem("mobius-stage-side");
  if (["right", "left", "bottom", "top"].includes(saved)) stageDock = saved;
} catch { /* not remembered */ }
const stageVertical = () => stageDock === "bottom" || stageDock === "top";
function stageSizeKey() { return (stageVertical() ? "mobius-stage-h-" : "mobius-stage-w-") + workspace; }
function setStageWidth(px, remember) {
  const app = $("app"), name = stageVertical() ? "--stage-h" : "--stage-w";
  app.style.removeProperty("--stage-w"); app.style.removeProperty("--stage-h");
  if (px) app.style.setProperty(name, Math.round(px) + "px");
  if (remember) { try { px ? localStorage.setItem(stageSizeKey(), String(Math.round(px))) : localStorage.removeItem(stageSizeKey()); } catch { /* not remembered */ } }
}
function loadStageWidth() {
  let saved = 0;
  try { saved = Number(localStorage.getItem(stageSizeKey())) || 0; } catch { /* not remembered */ }
  setStageWidth(saved >= (stageVertical() ? STAGE_MIN_H : STAGE_MIN) ? saved : 0, false);
}
function stageMax() { return stageVertical() ? Math.max(STAGE_MIN_H, window.innerHeight * 0.7) : Math.max(STAGE_MIN, Math.min(window.innerWidth * 0.78, window.innerWidth - 56 - 340)); }
function clampStage(px) { return Math.max(stageVertical() ? STAGE_MIN_H : STAGE_MIN, Math.min(stageMax(), px)); }
function drawStageSide() {
  for (const [id] of DOCKS) document.body.classList.toggle("stage-" + id, stageDock === id && id !== "right");
  $("stage-resizer").setAttribute("aria-orientation", stageVertical() ? "horizontal" : "vertical");
  $("stage-side-btn").hidden = !document.body.classList.contains("stage-open");
}
function setDock(next) {
  if (!DOCKS.some(([id]) => id === next) || next === stageDock) return;
  stageDock = next;
  try { localStorage.setItem("mobius-stage-dock", next); } catch { /* not remembered */ }
  drawStageSide(); loadStageWidth();
}
// Moving the work area: drag the ⇄ handle to an edge of the window (a preview shows where it will sit), or click it for a menu.
function dockAt(x, y) {
  const fx = x / window.innerWidth, fy = y / window.innerHeight;
  if (fx < 0.22) return "left";
  if (fx > 0.78) return "right";
  if (fy < 0.3) return "top";
  if (fy > 0.7) return "bottom";
  return stageDock;
}
function openDockMenu() {
  document.querySelectorAll(".dock-menu").forEach((m) => m.remove());
  const box = $("stage-side-btn").getBoundingClientRect();
  const menu = el("div", "hx-menu dock-menu"); menu.dataset.test = "dock-menu"; menu.setAttribute("role", "menu");
  menu.style.right = (window.innerWidth - box.right) + "px"; menu.style.top = (box.bottom + 6) + "px"; menu.style.bottom = "auto"; menu.style.minWidth = "200px";
  for (const [id, label] of DOCKS) {
    const b = el("button", "hx-menu-item" + (id === stageDock ? " current" : ""));
    b.type = "button"; b.setAttribute("role", "menuitemradio"); b.setAttribute("aria-checked", String(id === stageDock)); b.dataset.test = "dock-" + id;
    b.append(el("span", "mi-text", label));
    b.onclick = () => { menu.remove(); setDock(id); };
    menu.append(b);
  }
  document.body.append(menu);
  const close = (event) => { if (!menu.contains(event.target)) { menu.remove(); document.removeEventListener("pointerdown", close, true); } };
  setTimeout(() => document.addEventListener("pointerdown", close, true), 0);
  menu.addEventListener("keydown", (event) => { if (event.key === "Escape") { menu.remove(); $("stage-side-btn").focus(); } });
  menu.querySelector(".current, button")?.focus();
}
(() => {
  const bar = $("stage-resizer"), host = $("stage-host"), handle = $("stage-side-btn");
  const current = () => { const r = host.getBoundingClientRect(); return stageVertical() ? r.height : r.width; };
  bar.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    bar.setPointerCapture(event.pointerId);
    bar.classList.add("dragging"); document.body.classList.add("stage-dragging");
  });
  bar.addEventListener("pointermove", (event) => {
    if (!bar.hasPointerCapture(event.pointerId)) return;
    const r = host.getBoundingClientRect();
    const px = stageDock === "left" ? event.clientX - r.left : stageDock === "right" ? r.right - event.clientX : stageDock === "top" ? event.clientY - r.top : r.bottom - event.clientY;
    setStageWidth(clampStage(px), false);
  });
  const done = (event) => {
    if (!bar.hasPointerCapture(event.pointerId)) return;
    bar.releasePointerCapture(event.pointerId);
    bar.classList.remove("dragging"); document.body.classList.remove("stage-dragging");
    setStageWidth(clampStage(current()), true);
  };
  bar.addEventListener("pointerup", done); bar.addEventListener("pointercancel", done);
  bar.addEventListener("dblclick", () => setStageWidth(0, true));
  bar.addEventListener("keydown", (event) => {
    const [more, less] = stageDock === "left" ? ["ArrowRight", "ArrowLeft"] : stageDock === "right" ? ["ArrowLeft", "ArrowRight"] : stageDock === "top" ? ["ArrowDown", "ArrowUp"] : ["ArrowUp", "ArrowDown"];
    if (event.key !== more && event.key !== less && event.key !== "Home" && event.key !== "Enter") return;
    event.preventDefault();
    if (event.key === "Home" || event.key === "Enter") { setStageWidth(0, true); return; }
    setStageWidth(clampStage(current() + (event.key === more ? 32 : -32) * (event.shiftKey ? 4 : 1)), true);
  });
  // The handle: a press that moves is a drag to an edge, a press that does not is a click for the menu.
  let press = null, preview = null;
  handle.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    press = { x: event.clientX, y: event.clientY, moved: false }; handle.setPointerCapture(event.pointerId);
  });
  handle.addEventListener("pointermove", (event) => {
    if (!press) return;
    if (!press.moved && Math.hypot(event.clientX - press.x, event.clientY - press.y) < 8) return;
    press.moved = true;
    if (!preview) { preview = el("div", "dock-preview"); preview.dataset.test = "dock-preview"; document.body.append(preview); document.body.classList.add("stage-dragging"); }
    const target = dockAt(event.clientX, event.clientY);
    preview.dataset.dock = target;
  });
  const release = (event) => {
    if (!press) return;
    const was = press; press = null;
    if (handle.hasPointerCapture(event.pointerId)) handle.releasePointerCapture(event.pointerId);
    if (preview) { const target = preview.dataset.dock; preview.remove(); preview = null; document.body.classList.remove("stage-dragging"); if (target) setDock(target); }
    else if (!was.moved && event.type === "pointerup") openDockMenu();
  };
  handle.addEventListener("pointerup", release); handle.addEventListener("pointercancel", release);
  handle.addEventListener("keydown", (event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); openDockMenu(); } });
  stageReady = true; stageWidthFor = workspace; loadStageWidth(); drawStageSide();
  window.addEventListener("resize", () => { const app = $("app"); if (getComputedStyle(app).getPropertyValue(stageVertical() ? "--stage-h" : "--stage-w")) setStageWidth(clampStage(current()), false); });
})();
const reactStage = window.HelixMobiusReact?.stage($("stage-doc"), {
  chatId: () => view.id,
  report: (error) => report(error),
  changed: (open) => { if (!stageOpen && open) stageAttach = true; stageOpen = open; drawStageChip(); },
});

const reactCode = window.HelixMobiusReact?.codeStage($("stage-code"), {
  report: (error) => report(error),
  folder: () => view.workspace || "",
  chooseFolder: () => openTools(),
  changes: () => {
    for (let i = view.messages.length - 1; i >= 0; i--) {
      const a = (view.messages[i].activities || []).find((x) => x.tool === "changes");
      if (a) { try { return { activityId: a.id, files: JSON.parse(a.input) }; } catch { return null; } }
      if (view.messages[i].role === "user") break;
    }
    return null;
  },
  shown: (open) => { stageOpen = open; stageAttach = true; drawStageChip(); },
  tasks: () => codeTasks(),
  reveal: (index) => document.querySelector(`[data-test="msg-${index}"]`)?.scrollIntoView({ block: "start", behavior: "smooth" }),
  terminal: () => codeTerminal(),
});
function codeTerminal() {
  for (let i = view.messages.length - 1; i >= 0; i--) {
    const acts = view.messages[i].activities || [];
    for (let j = acts.length - 1; j >= 0; j--) {
      if (acts[j].tool === "terminal") return { command: acts[j].input || "", state: acts[j].state || "", output: acts[j].output || "" };
    }
  }
  return null;
}
function openProjectFile(path) {
  if (!path) return;
  setWorkspace("code");
  reactCode?.open();
  setTimeout(() => reactCode?.show(path), 0);
}
// Each request in this chat as a task: its state and what the agent did for it (the Code stage's Tasks list, and what Follow watches).
function codeTasks() {
  const out = [], m = view.messages;
  for (let i = 0; i < m.length; i++) {
    if (m[i].role !== "user") continue;
    const reply = m[i + 1] && m[i + 1].role === "assistant" ? m[i + 1] : null;
    const acts = (reply && reply.activities) || [];
    const live = view.streaming && (reply ? i + 1 === m.length - 1 : i === m.length - 1);
    const state = acts.some((a) => a.state === "waiting") ? "waiting" : live ? "running" : acts.some((a) => a.state === "failed") ? "failed" : reply ? "done" : "pending";
    const work = acts.filter((a) => a.tool !== "changes" && a.tool !== "planning");
    let changed = 0;
    const summary = acts.find((a) => a.tool === "changes");
    if (summary) { try { changed = JSON.parse(summary.input).length; } catch { /* no count */ } }
    else changed = acts.filter((a) => a.tool === "edit_file" && a.state === "completed").length;
    out.push({ index: i, request: m[i].text || "", state, steps: work.length, changed, inputs: work.map((a) => a.input || "") });
  }
  return out;
}
const reactImage = window.HelixMobiusReact?.imageStage($("stage-image"), {
  chatId: () => view.id,
  report: (error) => report(error),
  ask: (id) => addPictures([id]),
  askAbout: (id, text) => {
    addPictures([id]);
    let tries = 0;
    const go = () => {
      if (pendingPictures.some((p) => p.id === id)) { input.value = text; grow(); $("composer").requestSubmit(); }
      else if (++tries < 40) setTimeout(go, 50);
    };
    go();
  },
  active: () => {},
});
function setWorkspace(next, doc) {
  if (next === workspace && !(doc && (next === "write" || next === "image"))) return;
  if (next === workspace && next === "image") { reactImage?.open(doc, "Picture"); return; }
  if (next === workspace) { reactStage?.open(doc); return; }
  if (workspace === "write") reactStage?.close();
  if (workspace === "code") reactCode?.close();
  if (workspace === "image") reactImage?.close();
  workspace = next;
  if (VIEW_LABELS[next] && !openViews.includes(next)) { openViews.push(next); rememberViews(); }
  stageOpen = null;
  if (next === "code") reactCode?.open();
  if (next === "image") reactImage?.open(doc, "Picture");
  if (next === "write") { if (doc) reactStage?.open(doc); else reactStage?.open("Untitled", ""); }
  drawStageChip();
  drawApprovalHint();
  drawList();
}
function openWorkspaceMenu() {
  document.querySelectorAll(".workspace-menu").forEach((m) => m.remove());
  const box = $("workspace-btn").getBoundingClientRect();
  const menu = el("div", "hx-menu workspace-menu");
  menu.dataset.test = "workspace-menu";
  menu.style.right = (window.innerWidth - box.right) + "px";
  menu.style.top = (box.bottom + 6) + "px";
  menu.style.bottom = "auto";
  menu.style.minWidth = "260px";
  const row = (id, title, sub, enabled) => {
    const b = el("button", "hx-menu-item" + (workspace === id ? " current" : ""));
    b.type = "button";
    b.disabled = !enabled;
    b.dataset.test = "workspace-" + id;
    const text = el("span", "mi-text");
    text.append(el("span", "mi-title", title), el("span", "mi-sub", sub));
    b.append(text);
    if (workspace === id) b.append(el("span", "mi-check", "✓"));
    b.onclick = () => { menu.remove(); setWorkspace(id); };
    menu.append(b);
  };
  row("chat", "Chat", "Just the conversation", true);
  row("write", "Write", "A document beside the chat; Mobius drafts, you edit", true);
  row("image", "Image", "Edit a picture: crop, rotate, adjust, annotate", true);
  row("code", "Code", "Your project's files and what changed, beside the chat", true);
  const soon = el("div", "hx-menu-label", "Planned: Image · Design · Browse");
  menu.append(soon);
  document.body.append(menu);
  const close = (event) => { if (!menu.contains(event.target)) { menu.remove(); window.removeEventListener("mousedown", close, true); } };
  setTimeout(() => window.addEventListener("mousedown", close, true), 0);
}
$("workspace-btn").onclick = () => openWorkspaceMenu();
for (const id of ["chat", "write", "image", "code"]) $("mode-" + id).onclick = () => { $("mode-analyze").classList.remove("active"); setWorkspace(id); };

// Artifacts: every document the chats made, as a list in place of the chat list.
let artifactsView = false;
function drawArtifactsRow() {
  $("mode-artifacts").classList.toggle("active", artifactsView);
  $("chat-list-host").style.display = artifactsView ? "none" : "contents";
  $("artifacts-list").hidden = !artifactsView;
}
async function showArtifacts(on) {
  artifactsView = on;
  drawArtifactsRow();
  if (!on) return;
  const host = $("artifacts-list");
  host.replaceChildren();
  const rows = await invoke("stage_all").catch(() => []);
  if (!rows.length) { host.append(el("p", "artifacts-empty", "Nothing yet. When Mobius writes a document in a reply it appears here, with the chat it came from.")); return; }
  rows.forEach((row, i) => {
    const b = el("button", "artifact-row");
    b.type = "button";
    b.dataset.test = "artifact-row-" + i;
    b.append(el("strong", "", row.title), el("span", "", `${row.chat_title} · ${row.words} words · ${row.versions} version${row.versions === 1 ? "" : "s"}`));
    b.onclick = () => openArtifactRow(row);
    host.append(b);
  });
}
async function openArtifactRow(row) {
  artifactsView = false;
  drawArtifactsRow();
  if (view.id !== row.chat_id) {
    await invoke("chat_open", { id: row.chat_id }).catch(report);
    for (let i = 0; i < 40 && view.id !== row.chat_id; i++) await new Promise((r) => setTimeout(r, 50));
  }
  setWorkspace("write", row.title);
}
$("mode-artifacts").onclick = () => showArtifacts(!artifactsView);

// A side chat: a temporary chat that has been given the main chat to read, so a question can be asked without touching it.
let sideOf = null;
function drawSideChip() {
  const chip = $("side-chip");
  chip.hidden = !(sideOf && view.temporary);
  if (!chip.hidden) chip.textContent = `Side chat · reads “${sideOf.title}” · back to it`;
}
async function startSideChat() {
  if (!view.id || !view.messages.length) { report("Start a chat first; a side chat reads the one you are in."); return; }
  if (view.temporary) { report("This is already a temporary chat."); return; }
  const main = { id: view.id, title: view.title || "this chat" };
  const context = await invoke("mentions_resolve", { kind: "chat", key: main.id });
  await invoke("chat_temporary");
  sideOf = main;
  pendingAttachments = [context];
  drawAttachChips();
  drawSideChip();
  input.focus();
}
$("side-chip").onclick = () => { const id = sideOf && sideOf.id; sideOf = null; drawSideChip(); if (id) invoke("chat_open", { id }).catch(report); };
// A reply that wrote documents: keep each as a version, then show the newest text if that document is open.
async function recordArtifacts() {
  const last = view.messages[view.messages.length - 1];
  if (!last || last.role !== "assistant" || !view.id) return;
  const found = [...last.text.matchAll(/```artifact:([^\n]+)\n([\s\S]*?)```/g)];
  for (const m of found) await invoke("stage_record", { chatId: view.id, title: m[1].trim(), body: m[2].replace(/\n$/, ""), by: "model" }).catch(() => undefined);
  if (found.length) reactStage?.refresh();
}
const reactPalette = window.HelixMobiusReact?.palette($("palette-host"), {
  items: (query) => paletteItems(query),
  openChat: (id) => invoke("chat_open", { id }).then(() => { clearAttachments(); input.focus(); }).catch(report),
  newChatWith: (text) => invoke("chat_new").then(() => { input.value = text; grow(); syncSendEnabled(); input.focus(); }).catch(report),
});

function openTools() { reactTools?.open({ ...view, craft: workspace }); }
$("meta-tools").onclick = () => openTools();
$("meta-agent-desk").onclick = () => invoke("agent_desk_focus").then(refreshAgentDeskGlance).catch(report);
