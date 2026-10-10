// Helix System Monitor. Host owns polls; this page only draws the latest view as text.
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

let page = "overview";
const TITLES = { overview: "Overview", processes: "Processes", "screen-time": "Screen Time" };

function pct(n) { return (Math.round(n * 10) / 10).toFixed(1) + "%"; }
function bytes(n) {
  if (n >= 1e9) return (n / 1e9).toFixed(1) + " GB";
  if (n >= 1e6) return (n / 1e6).toFixed(1) + " MB";
  return n + " B";
}

async function draw() {
  const view = await invoke("monitor_view", { query: $("search").value || "", sort: "cpu" });
  $("page-title").textContent = TITLES[page];
  $("search").hidden = page !== "processes";
  await invoke("monitor_set_gpu", { shown: page === "overview" });
  const body = $("body");
  body.replaceChildren();
  if (page === "overview") {
    const card = el("div", "rows-card");
    const rows = [
      ["CPU", pct(view.overview.cpu_percent), "stat-cpu"],
      ["Memory", pct(view.overview.mem_percent) + " · " + bytes(view.overview.mem_used) + " / " + bytes(view.overview.mem_total), "stat-mem"],
      ["Swap", pct(view.overview.swap_percent), "stat-swap"],
      ["Network", "↓ " + bytes(view.overview.net_rx) + "  ↑ " + bytes(view.overview.net_tx), "stat-net"],
      ["Load", view.overview.load_1 + " / " + view.overview.load_5 + " / " + view.overview.load_15, "stat-load"],
      ["GPU", view.gpu_shown ? "Probing while Overview is open" : "Hidden", "stat-gpu"],
    ];
    rows.forEach(([title, value, test]) => {
      const line = el("div", "setting-row");
      line.dataset.test = test;
      const text = el("div", "setting-text");
      text.append(el("div", "setting-title", title));
      text.append(el("div", "setting-about", value));
      line.append(text);
      card.append(line);
    });
    body.append(card);
  } else if (page === "processes") {
    const card = el("div", "rows-card");
    view.processes.slice(0, 80).forEach((row) => {
      const line = el("div", "setting-row");
      line.dataset.test = "proc:" + row.pid;
      const text = el("div", "setting-text");
      text.append(el("div", "setting-title", row.name + " · " + row.pid));
      text.append(el("div", "setting-about", "RSS " + bytes(row.rss) + " · threads " + row.threads));
      line.append(text);
      card.append(line);
    });
    if (!view.processes.length) body.append(el("div", "empty", "No processes."));
    else body.append(card);
  } else {
    const card = el("div", "rows-card");
    view.screen_time.forEach((row) => {
      const line = el("div", "setting-row");
      line.dataset.test = "screen:" + row.app;
      const text = el("div", "setting-text");
      text.append(el("div", "setting-title", row.app));
      text.append(el("div", "setting-about", Math.round(row.seconds / 60) + " min"));
      line.append(text);
      card.append(line);
    });
    if (!view.screen_time.length) body.append(el("div", "empty", "No screen-time report yet."));
    else body.append(card);
  }
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
$("search").oninput = () => draw();

(async () => {
  window.__layout = await invoke("layout_reporting");
  const start = new URLSearchParams(location.search).get("page");
  if (start && TITLES[start]) {
    page = start;
    document.querySelectorAll(".page-link").forEach((n) => n.classList.toggle("current", n.dataset.page === page));
  }
  await listen("monitor", () => draw());
  await draw();
  if (window.__layout) setInterval(report, 500);
})();
