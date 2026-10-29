// Mounts the React islands when their bundle is there (shell/helix-shell-tauri/web). Without it the hand-written drawing in shell.js stays
// in charge. Loaded after both shell.js and the bundle.
if (window.HelixShellReact) {
  reactNotifications = window.HelixShellReact.notifications($("toasts"), $("notif-list"));
  if (lastNotifications) reactNotifications.update(lastNotifications);
  reactLauncher = window.HelixShellReact.launcher($("launcher"));
  reactToday = window.HelixShellReact.today($("today"));
  reactControls = window.HelixShellReact.controls($("controls"));
  reactTray = window.HelixShellReact.tray($("tray"), $("tray-menu"), {
    context: (item, button, at) => openTrayMenu(item, button, at),
  });
  reactDock = window.HelixShellReact.dock($("dock-items"), {
    menu: (item, cell) => openDockMenu(item, cell),
    tip: (item, cell) => showDockTip(item.unverified ? item.detail : item.name, cell),
    untip: () => $("dock-tip").classList.remove("show"),
  });
  if (dockItems.length) reactDock.set(dockItems);
  reactIsland = window.HelixShellReact.island($("island"));
  reactOsd = window.HelixShellReact.osd($("osd"));
  reactMobius = window.HelixShellReact.mobius($("panel"));
  reactQuickInsert = window.HelixShellReact.quickInsert($("quickinsert"));
  if (window.HelixShellReact.writing) reactWriting = window.HelixShellReact.writing($("writing"));
  if (window.HelixShellReact.pointer) reactPointer = window.HelixShellReact.pointer($("pointer"));
}
// React updates its islands without going through shell.js's draw functions, so the test layout report is asked for after they change.
if (window.HelixShellReact) {
  const again = new MutationObserver(() => queueReport());
  ["launcher", "today", "toasts", "notif-list", "controls", "quickinsert", "writing", "pointer", "panel", "island", "osd", "tray", "tray-menu", "dock-items"].forEach((id) => again.observe($(id), { childList: true, subtree: true }));
}
