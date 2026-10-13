const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);

listen("admin-auth-view", (event) => {
  $("admin-error").textContent = "";
  $("admin-password").value = "";
  $("subject").textContent = event.payload.prompt || "Authentication is required.";
  $("admin-password").focus();
});

$("cancel").onclick = () => invoke("admin_auth_cancel").catch(() => undefined);
$("ok").onclick = () => {
  $("admin-error").textContent = "";
  invoke("admin_auth_submit", { password: $("admin-password").value }).catch((error) => {
    $("admin-error").textContent = String(error || "Could not send the password");
  });
};
$("admin-password").addEventListener("keydown", (event) => {
  if (event.key === "Enter") $("ok").click();
  if (event.key === "Escape") $("cancel").click();
});
