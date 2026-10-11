// Mounts the React islands when their bundle is there (shell/helix-shell-tauri/web). Without it the hand-written drawing in shell.js stays
// in charge. Loaded after both shell.js and the bundle.
if (window.HelixShellReact) {
  reactNotifications = window.HelixShellReact.notifications($("toasts"), $("notif-list"));
  if (lastNotifications) reactNotifications.update(lastNotifications);
  reactLauncher = window.HelixShellReact.launcher($("launcher"));
  reactToday = window.HelixShellReact.today($("today"));
}
// React updates its islands without going through shell.js's draw functions, so the test layout report is asked for after they change.
if (window.HelixShellReact) {
  const again = new MutationObserver(() => queueReport());
  ["launcher", "today", "toasts", "notif-list"].forEach((id) => again.observe($(id), { childList: true, subtree: true }));
}
