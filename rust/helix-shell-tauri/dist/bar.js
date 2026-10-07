const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const live = document.getElementById("live");
listen("live-value", (event) => {
  live.textContent = String(event.payload);
});
document.getElementById("ask").onclick = () => invoke("panel_open");
requestAnimationFrame(() => invoke("ready"));
