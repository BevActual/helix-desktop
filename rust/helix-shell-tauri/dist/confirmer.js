// Agent1 confirmer surface. Trusted text only via textContent. Enter never calls Allow.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const title = document.getElementById("title");
const subject = document.getElementById("subject");
const detail = document.getElementById("detail");
const aside = document.getElementById("aside");
const tiers = document.getElementById("tiers");
const warning = document.getElementById("warning");
const waiting = document.getElementById("waiting");
const allow = document.getElementById("allow");
const deny = document.getElementById("deny");

function show(view) {
  if (!view || !view.open) {
    title.textContent = "";
    subject.textContent = "";
    detail.textContent = "";
    aside.textContent = "";
    waiting.textContent = "";
    warning.classList.remove("show");
    tiers.replaceChildren();
    allow.disabled = true;
    return;
  }
  const grant = view.kind === "grant";
  title.textContent = grant ? "Allow access?" : "Allow this action?";
  subject.textContent = grant
    ? view.client_name + " is asking for access"
    : view.client_name + " wants to run a command";
  detail.textContent = view.detail || "";
  aside.textContent = view.aside || "";
  warning.classList.toggle("show", !!view.tainted);
  waiting.textContent = view.waiting > 0 ? view.waiting + " more waiting" : "";
  allow.disabled = !view.armed;
  tiers.replaceChildren();
  (view.tiers || []).forEach((tier) => {
    const row = document.createElement("label");
    row.className = "tier";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = !!tier.checked;
    box.dataset.tier = tier.name;
    box.setAttribute("data-test", "confirm-tier-" + tier.name);
    box.addEventListener("change", () => {
      invoke("confirmer_set_tier", { name: tier.name, checked: box.checked }).then(show);
    });
    const name = document.createElement("span");
    name.textContent = tier.name;
    row.append(box, name);
    tiers.append(row);
  });
}

deny.onclick = () => invoke("confirmer_deny").then((view) => { show(view); queueReport(); });
allow.onclick = () => invoke("confirmer_allow").then((view) => { show(view); queueReport(); });
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") invoke("confirmer_escape");
  if (event.key === "Enter") {
    event.preventDefault();
    invoke("confirmer_enter").then(show);
  }
});

let reporting = false;
let reportTimer = null;
let reportPulse = null;
function queueReport() {
  if (!reporting) return;
  clearTimeout(reportTimer);
  reportTimer = setTimeout(sendReport, 80);
}
function sendReport() {
  const elements = {};
  document.querySelectorAll("[data-test]").forEach((element) => {
    const box = element.getBoundingClientRect();
    if (box.width > 0 && box.height > 0) {
      elements[element.dataset.test] = { x: box.x, y: box.y, w: box.width, h: box.height };
    }
  });
  invoke("report_layout", {
    report: { width: window.innerWidth, height: window.innerHeight, panel: "confirmer", elements },
  }).catch(() => {});
}
window.addEventListener("resize", queueReport);

// Register the listener before ready(), otherwise the replayed view is lost.
listen("confirmer-view", (event) => {
  show(event.payload);
  queueReport();
}).then(() => {
  invoke("layout_reporting")
    .then((wanted) => {
      reporting = !!wanted;
      queueReport();
      if (reporting && !reportPulse) {
        reportPulse = setInterval(queueReport, 250);
      }
    })
    .catch(() => {});
  invoke("ready");
});
