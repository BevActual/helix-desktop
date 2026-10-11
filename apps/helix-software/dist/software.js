// Helix Software. Draws package views as text only; mutations go through request → confirm.
// With the React bundle present it draws everything; the hand-written page below is the fallback for a build without node.
if (window.HelixSoftwareReact) {
  window.HelixSoftwareReact.mount(document.getElementById("app"));
} else {
(function legacy() {
const { invoke } = window.__TAURI__.core;
const $ = (id) => document.getElementById(id);
function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
$("close").onclick = () => window.__TAURI__.window.getCurrentWindow().close();

let page = "explore";
let kind = "flatpak";
const TITLES = { explore: "Explore", library: "Library", updates: "Updates", sources: "Sources" };

async function draw() {
  const query = $("search").value || "";
  const view = await invoke("software_view", { page, query, kind });
  $("page-title").textContent = TITLES[page];
  const list = $("list");
  list.replaceChildren();
  const confirm = $("confirm");
  if (view.pending) {
    confirm.hidden = false;
    confirm.replaceChildren();
    const note = el("div", "setting-about", "Confirm " + view.pending);
    const yes = el("button", "segment", "Allow");
    yes.dataset.test = "confirm-yes";
    yes.onclick = () => invoke("software_confirm", { accept: true }).then(draw);
    const no = el("button", "segment", "Cancel");
    no.dataset.test = "confirm-no";
    no.onclick = () => invoke("software_confirm", { accept: false }).then(draw);
    const row = el("div", "setting-row");
    row.append(yes, no, note);
    confirm.append(row);
  } else {
    confirm.hidden = true;
    confirm.replaceChildren();
  }
  if (page === "sources") {
    const card = el("div", "rows-card");
    view.sources.forEach((name) => {
      const line = el("div", "setting-row");
      line.dataset.test = "source:" + name;
      line.append(el("div", "setting-title", name));
      card.append(line);
    });
    list.append(card);
    report();
    return;
  }
  const rows = page === "explore" ? view.explore : page === "library" ? view.library : view.updates;
  if (!rows.length) {
    list.append(el("div", "empty", "Nothing here."));
    report();
    return;
  }
  const card = el("div", "rows-card");
  rows.forEach((row) => {
    const line = el("div", "setting-row");
    line.dataset.test = "pkg:" + row.id;
    const action = row.update ? "update" : row.installed ? "remove" : "install";
    const button = el("button", "segment pkg-action", action === "install" ? "Install" : action === "remove" ? "Remove" : "Update");
    button.dataset.test = "action:" + row.id;
    button.onclick = () => invoke("software_request", { id: row.id, action }).then(draw).catch(console.error);
    const text = el("div", "setting-text");
    text.append(el("div", "setting-title", row.title || row.id));
    if (row.summary) text.append(el("div", "setting-about", row.summary));
    // Action first + margin-right auto so nested pointer hits stay on the left of wide rows.
    line.append(button, text);
    card.append(line);
  });
  list.append(card);
  report();
}

function report() {
  if (!window.__layout) return;
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((node) => {
    const box = node.getBoundingClientRect();
    if (box.width > 0 && box.height > 0 && getComputedStyle(node).visibility !== "hidden") {
      elements[node.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
    }
  });
  invoke("report_layout", { report: { width: innerWidth, height: innerHeight, elements } });
}

document.querySelectorAll(".page-link").forEach((link) => {
  link.onclick = () => {
    page = link.dataset.page;
    document.querySelectorAll(".page-link").forEach((n) => n.classList.toggle("current", n === link));
    draw();
  };
});
$("search").oninput = () => { if (page === "explore") draw(); };

(async () => {
  window.__layout = await invoke("layout_reporting");
  const params = new URLSearchParams(location.search);
  const deep = params.get("app");
  const start = params.get("page");
  if (deep) {
    const detail = await invoke("software_detail", { id: deep });
    if (detail) {
      page = detail.installed ? "library" : "explore";
      document.querySelectorAll(".page-link").forEach((n) => n.classList.toggle("current", n.dataset.page === page));
    }
  } else if (start && TITLES[start]) {
    page = start;
    document.querySelectorAll(".page-link").forEach((n) => n.classList.toggle("current", n.dataset.page === page));
  }
  await draw();
  if (window.__layout) setInterval(report, 500);
})();
})();
}
