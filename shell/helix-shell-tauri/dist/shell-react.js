// Mounts the React islands when their bundle is there (shell/helix-shell-tauri/web). Without it the hand-written drawing in shell.js stays
// in charge. Loaded after both shell.js and the bundle.
if (window.HelixShellReact) {
  reactNotifications = window.HelixShellReact.notifications($("toasts"), $("notif-list"));
  if (lastNotifications) reactNotifications.update(lastNotifications);
}
