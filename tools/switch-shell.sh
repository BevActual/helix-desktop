#!/bin/bash
# Install or refresh the Tauri session shell. The C++ session shell is retired; there is no `native` switch-back.
#   tools/switch-shell.sh                same as install (no shell restart)
#   tools/switch-shell.sh install        write units + links + enable at login; does NOT restart live shells
#   tools/switch-shell.sh restart-panels bounce Overlay panels only (safe while Cursor is open)
#   tools/switch-shell.sh restart-bar    bounce Top bar only (panels revive via Wants/Upholds; can disrupt Cursor)
#   tools/switch-shell.sh restart        bounce bar+panels (or legacy monolith). Avoid while Cursor is open.
#   tools/switch-shell.sh status
# Key binds call ~/.local/bin/helix-panel → Shell1 on the bar.
#
# Split: helix-shell-bar (Top / exclusive zone) + helix-shell-panels (Overlay). Restarting panels does not
# tear down the bar. Restarting the bar still can disrupt Cursor — prefer restart-panels for UI work.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UNITS=$HOME/.config/systemd/user

pick_bin() {
  local name=$1 build
  for build in release fast; do
    if [ -x "$ROOT/target/$build/$name" ]; then
      printf '%s' "$ROOT/target/$build/$name"
      return 0
    fi
  done
  return 1
}

link_app() {
  local name=$1 desktop=$2
  local bin
  bin=$(pick_bin "$name") || return 0
  mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications"
  if [ -e "$HOME/.local/bin/$name" ] && [ ! -L "$HOME/.local/bin/$name" ]; then
    mv -n "$HOME/.local/bin/$name" "$HOME/.local/bin/${name}-cpp" 2>/dev/null || true
  fi
  ln -sf "$bin" "$HOME/.local/bin/$name"
  if [ -n "$desktop" ] && [ -f "$ROOT/assets/$desktop" ]; then
    ln -sf "$ROOT/assets/$desktop" "$HOME/.local/share/applications/$desktop"
  fi
}

write_units_and_links() {
  BIN=$(pick_bin helix-shell-tauri) || {
    echo "build it first: (cargo build --release -p helix-shell-tauri)" >&2
    exit 1
  }
  mkdir -p "$UNITS" "$HOME/.local/bin"
  install -m 0755 "$ROOT/tools/helix-panel" "$HOME/.local/bin/helix-panel"
  link_app helix-settings org.helix.Settings.desktop || true
  if [ -L "$HOME/.local/bin/helix-settings" ] || [ -x "$HOME/.local/bin/helix-settings" ]; then
    ln -sf "$(readlink -f "$HOME/.local/bin/helix-settings" 2>/dev/null || echo "$HOME/.local/bin/helix-settings")" \
      "$HOME/.local/bin/helix-settings-new" 2>/dev/null || true
  fi
  link_app helix-software org.helix.Software.desktop
  link_app helix-system-monitor org.helix.SystemMonitor.desktop
  link_app helix-mobius org.helix.Mobius.desktop || true
  # Transition alias until launchers finish migrating from helix-chat.
  if [ -L "$HOME/.local/bin/helix-mobius" ] || [ -x "$HOME/.local/bin/helix-mobius" ]; then
    ln -sf "$(readlink -f "$HOME/.local/bin/helix-mobius" 2>/dev/null || echo "$HOME/.local/bin/helix-mobius")" \
      "$HOME/.local/bin/helix-chat" 2>/dev/null || true
  fi
  escaped=$(printf '%s' "$BIN" | sed 's/ /\\\\x20/g')
  # Legacy monolith unit (kept for nested tests / rollback).
  sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-tauri.service.in" > "$UNITS/helix-shell-tauri.service"
  sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-bar.service.in" > "$UNITS/helix-shell-bar.service"
  sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-panels.service.in" > "$UNITS/helix-shell-panels.service"
  python3 - "$HOME/.config/niri/config.kdl" "$HOME/.config/niri/helix-setup.kdl" "$HOME/.config/niri/helix-binds.kdl" <<'PY'
import os, shutil, sys, time
panel = '"' + os.path.expanduser('~/.local/bin/helix-panel') + '"'
names = {"panel-toggle launcher": "launcher", "panel-toggle control-center": "control-center", "settings-toggle": "settings",
         "window-switcher": "window-switcher", "volume-up": "volume-up", "volume-down": "volume-down", "volume-mute": "volume-mute",
         "mic-mute": "mic-mute", "brightness-up": "brightness-up", "brightness-down": "brightness-down",
         "session lock": "lock", "session lid-close": "lid-close", "panel-toggle notifications": "notifications",
         "panel-toggle quick-insert": "quick-insert", "notification-dnd-toggle": "dnd", "nightlight-toggle": "night-light",
         "media toggle": "media-toggle", "media next": "media-next", "media previous": "media-previous",
         "screenshot-region": "screenshot", "screenshot-fullscreen": "screenshot-screen"}
swaps = {}
for native, name in names.items():
    old = "spawn " + " ".join('"%s"' % word for word in ["helix", "msg"] + native.split())
    swaps[old] = "spawn %s \"%s\"" % (panel, name)
    swaps['spawn-sh "helix-panel ' + native + '"'] = "spawn %s \"%s\"" % (panel, name)
for path in sys.argv[1:]:
    if not os.path.isfile(path):
        continue
    text = open(path).read()
    new = text
    for old, replacement in swaps.items():
        new = new.replace(old, replacement)
    if new != text:
        shutil.copy2(path, "%s.bak-helix-%s" % (path, time.strftime("%Y%m%d-%H%M%S")))
        open(path, "w").write(new)
        print("key binds changed in", path)
PY
  if WALL=$(pick_bin helix-wallpaper); then
    wall_escaped=$(printf '%s' "$WALL" | sed 's/ /\\\\x20/g')
    sed "s|@WALLPAPER_BIN@|$wall_escaped|" "$ROOT/tools/systemd/helix-wallpaper.service.in" > "$UNITS/helix-wallpaper.service"
  fi
  if IDLE=$(pick_bin helix-idle); then
    idle_escaped=$(printf '%s' "$IDLE" | sed 's/ /\\\\x20/g')
    sed "s|@IDLE_BIN@|$idle_escaped|" "$ROOT/tools/systemd/helix-idle.service.in" > "$UNITS/helix-idle.service"
  fi
  systemctl --user daemon-reload
  systemctl --user stop helix-shell-tauri-trial.service 2>/dev/null || true
  systemctl --user disable helix-shell-tauri-trial.service 2>/dev/null || true
  # Retire C++ session unit without stopping a live Tauri shell; drop leftover unit files.
  systemctl --user disable helix-shell-dev.service 2>/dev/null || true
  systemctl --user stop helix-shell-dev.service 2>/dev/null || true
  systemctl --user mask helix-shell-dev.service 2>/dev/null || true
  rm -f "$UNITS/helix-shell-dev.service" "$UNITS/helix-shell-tauri-trial.service"
  rm -rf "$UNITS/helix-shell-dev.service.d"
  systemctl --user daemon-reload
  # Prefer split bar+panels; leave monolith unit installed but disabled when split is enabled.
  systemctl --user enable helix-shell-bar.service helix-shell-panels.service
  systemctl --user disable helix-shell-tauri.service 2>/dev/null || true
  if [ -f "$UNITS/helix-idle.service" ]; then
    systemctl --user enable helix-idle.service
  fi
  if [ -f "$UNITS/helix-wallpaper.service" ]; then
    systemctl --user enable helix-wallpaper.service
  fi
}

shell_live() {
  systemctl --user is-active --quiet helix-shell-bar.service \
    || systemctl --user is-active --quiet helix-shell-tauri.service \
    || systemctl --user is-active --quiet helix-shell-panels.service
}

case "${1:-install}" in
  install|tauri)
    write_units_and_links
    # If any shell half is running, never bounce from install.
    if shell_live; then
      echo "Links and units updated. Shell already running — not restarted (avoids crashing Cursor)."
      echo "UI / panels bounce: $0 restart-panels"
      echo "Bar-only bounce (can disrupt Cursor): $0 restart-bar"
      echo "Full pair bounce (can disrupt Cursor): $0 restart"
    else
      systemctl --user start helix-shell-bar.service helix-shell-panels.service
      if [ -f "$UNITS/helix-idle.service" ]; then
        systemctl --user start helix-idle.service 2>/dev/null || true
      fi
      if [ -f "$UNITS/helix-wallpaper.service" ]; then
        systemctl --user start helix-wallpaper.service 2>/dev/null || true
      fi
      echo "Helix Shell started (bar + panels)."
    fi
    ;;
  restart-panels)
    write_units_and_links
    echo "Restarting helix-shell-panels only (bar exclusive zone stays up)…"
    systemctl --user restart helix-shell-panels.service
    echo "Panels restarted."
    ;;
  restart-bar)
    write_units_and_links
    echo "Restarting helix-shell-bar (can disrupt Cursor)…"
    systemctl --user stop helix-shell-tauri.service 2>/dev/null || true
    systemctl --user restart helix-shell-bar.service
    # BindsTo stops panels with the bar; Wants/Upholds should revive them — start explicitly anyway.
    systemctl --user start helix-shell-panels.service
    echo "Bar restarted; panels ensured."
    ;;
  restart)
    write_units_and_links
    echo "Restarting shell bar+panels (GUI apps on this session may briefly lose the bar)…"
    systemctl --user stop helix-shell-tauri.service 2>/dev/null || true
    systemctl --user restart helix-shell-bar.service helix-shell-panels.service
    if [ -f "$UNITS/helix-idle.service" ]; then
      systemctl --user restart helix-idle.service
    fi
    if [ -f "$UNITS/helix-wallpaper.service" ]; then
      systemctl --user restart helix-wallpaper.service
    fi
    echo "Restarted."
    ;;
  native)
    echo "The C++ session shell is retired. Use: $0 install" >&2
    exit 2
    ;;
  status)
    for unit in helix-shell-bar helix-shell-panels helix-shell-tauri helix-wallpaper helix-idle helix-service; do
      printf '%-22s %s / %s\n' "$unit" "$(systemctl --user is-active $unit.service 2>/dev/null || true)" "$(systemctl --user is-enabled $unit.service 2>/dev/null || echo disabled)"
    done
    if systemctl --user is-enabled helix-shell-dev.service >/dev/null 2>&1; then
      echo "helix-shell-dev        still enabled (legacy — run: $0 install)" >&2
    fi
    ;;
  *)
    echo "usage: $0 [install|restart-panels|restart-bar|restart|status]" >&2
    echo "  install         update links/units; start shell only if not already running" >&2
    echo "  restart-panels  bounce Overlay panels only (safe with Cursor open)" >&2
    echo "  restart-bar     bounce Top bar; ensures panels (can disrupt Cursor)" >&2
    echo "  restart         bounce bar+panels (can disrupt Cursor — prefer a terminal outside Cursor)" >&2
    exit 2
    ;;
esac
