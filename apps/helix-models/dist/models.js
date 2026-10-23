// Helix Models: the whole page is React (web/src/main.tsx, built into react/helix-models.js).
if (window.HelixModelsReact) {
  window.HelixModelsReact.mount(document.getElementById("app"));
} else {
  document.getElementById("app").textContent = "The Models page was built without its interface. Run `npm run build` in the workspace.";
}
