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
  const open = event.payload.panel === "assistant";
  document.body.classList.toggle("open", open);
  if (open) {
    $("q").value = "";
    $("q").focus();
  }
});

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
  if (event.key === "Escape") invoke("assistant_escape");
});
requestAnimationFrame(() => invoke("ready"));
