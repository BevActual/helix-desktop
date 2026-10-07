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
$("apps").onclick = () => invoke("panel_open", { id: "launcher" });

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
  if (event.key === "Escape") invoke("panel_escape");
});
requestAnimationFrame(() => invoke("ready"));
