// Agent1 confirmer surface. Trusted text only via textContent. Enter never calls Allow, and no key does.
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
const identity = document.getElementById("identity");
// The request on screen: every decision names it, so a click can only ever apply to what the person was looking at.
let shownOperation = 0;

function show(view) {
  if (!view || !view.open) {
    shownOperation = 0;
    identity.textContent = "";
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
  shownOperation = view.operation;
  identity.textContent = view.identity ? "Verified: " + view.identity : "";
  allow.title = view.incomplete ? "What would run is not shown in full, so it cannot be approved" : "";
  const grant = view.kind === "grant";
  title.textContent = grant ? "Allow access?" : "Allow this action?";
  subject.textContent = grant
    ? view.client_name + " is asking for access"
    : view.client_name + " wants to run a command";
  document.getElementById("detaillabel").textContent = grant ? "Request" : "Exact action";
  detail.textContent = view.detail || "";
  aside.textContent = view.aside || "";
  warning.classList.toggle("show", !!view.tainted);
  // What the compositor does for this dialog. Nothing is said when it is protected.
  const protection = document.getElementById("protection");
  protection.dataset.state = view.protection || "off";
  protection.textContent =
    view.protection === "unknown" ? "Checking that this dialog is protected…"
    : view.protection === "unprotected" ? "This desktop cannot protect approvals from other programs, so actions that need confirmation are refused here."
    : view.protection === "unprotected-allowed" ? "This desktop cannot protect this dialog from other programs. You have chosen to approve anyway."
    : "";
  waiting.textContent = view.waiting > 0 ? view.waiting + " more waiting" : "";
  allow.disabled = !view.armed;
  // Allow stays dead for the arming delay; the sweep shows that it is coming alive, not broken.
  allow.classList.toggle("arming", !view.armed);
  tiers.replaceChildren();
  (view.tiers || []).forEach((tier) => {
    const row = document.createElement("label");
    row.className = "tier";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = !!tier.checked;
    box.dataset.tier = tier.name;
    box.setAttribute("data-test", "confirm-tier-" + tier.name);
    box.className = "switch";
    box.addEventListener("change", () => {
      invoke("confirmer_set_tier", { operation: shownOperation, name: tier.name, checked: box.checked }).then(show);
    });
    const name = document.createElement("span");
    name.textContent = tier.name;
    row.append(box, name);
    tiers.append(row);
  });
}

deny.onclick = () => invoke("confirmer_deny", { operation: shownOperation }).then((view) => { show(view); queueReport(); });
// Only a pointer (or a touch) approves. A click that a key produced (Space or Enter on the focused button) has detail 0; it is ignored, so no
// keystroke, real or synthetic, can allow anything. The pointer is what the compositor protects (docs/design/trusted-surface.md).
allow.onclick = (event) => {
  if (event.detail === 0) return;
  invoke("confirmer_allow", { operation: shownOperation }).then((view) => { show(view); queueReport(); });
};
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
