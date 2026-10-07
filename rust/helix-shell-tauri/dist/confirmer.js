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

listen("confirmer-view", (event) => show(event.payload));
deny.onclick = () => invoke("confirmer_deny").then(show);
allow.onclick = () => invoke("confirmer_allow").then(show);
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") invoke("confirmer_escape");
  if (event.key === "Enter") {
    event.preventDefault();
    invoke("confirmer_enter").then(show);
  }
});
requestAnimationFrame(() => invoke("ready"));
