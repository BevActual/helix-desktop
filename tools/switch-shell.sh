#!/bin/bash
# Install or refresh the Tauri session shell. The C++ session shell is retired; there is no `native` switch-back.
#   tools/switch-shell.sh                same as install (no shell restart)
#   tools/switch-shell.sh install        write units + links + enable at login; does NOT restart live shells
#   tools/switch-shell.sh restart-panels bounce Overlay panels only (safe while Cursor is open)
#   tools/switch-shell.sh restart-bar    bounce painted Top bar; exclusive-zone strut stays up
#   tools/switch-shell.sh restart-strut  bounce exclusive-zone holder only (rare; flaps work area)
#   tools/switch-shell.sh restart        bounce bar+panels (strut stays). Avoid strut bounce while Cursor is open.
#   tools/switch-shell.sh status
# Key binds call ~/.local/bin/helix-panel → Shell1 on the bar.
#
# Split: helix-shell-strut (exclusive zone) + helix-shell-bar (painted Top) + helix-shell-panels (Overlay).
# Restarting panels or the bar does not tear down the strut. Restarting the strut flaps niri work area
# and can crash Cursor — same danger the old bar restart had.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UNITS=$HOME/.config/systemd/user

pick_bin() {
  local name=$1 build path mtime best= best_mtime=0
  for build in release fast; do
    path="$ROOT/target/$build/$name"
    if [ -x "$path" ]; then
      mtime=$(stat -c %Y "$path" 2>/dev/null || echo 0)
      if [ "$mtime" -gt "$best_mtime" ]; then
        best_mtime=$mtime
        best=$path
      fi
    fi
  done
  if [ -n "$best" ]; then
    printf '%s' "$best"
    return 0
  fi
  return 1
}

find_polkit_agent() {
  local p
  for p in \
    /usr/libexec/polkit-kde-authentication-agent-1 \
    /usr/libexec/kf6/polkit-kde-authentication-agent-1 \
    /usr/bin/polkit-kde-authentication-agent-1 \
    /usr/libexec/polkit-gnome-authentication-agent-1 \
    /usr/bin/polkit-gnome-authentication-agent-1; do
    if [ -x "$p" ]; then
      printf '%s' "$p"
      return 0
    fi
  done
  return 1
}

write_polkit_agent_unit() {
  # The Tauri bar registers a native agent; use HELIX_EXTERNAL_POLKIT=1 to wrap KDE/GNOME instead.
  if [ "${HELIX_EXTERNAL_POLKIT:-0}" != 1 ]; then
    systemctl --user disable helix-polkit-agent.service 2>/dev/null || true
    systemctl --user stop helix-polkit-agent.service 2>/dev/null || true
    rm -f "$UNITS/helix-polkit-agent.service"
    return 1
  fi
  local agent escaped
  if agent=$(find_polkit_agent); then
    escaped=$(printf '%s' "$agent" | sed 's/ /\\\\x20/g')
    sed "s|@POLKIT_AGENT@|$escaped|" "$ROOT/tools/systemd/helix-polkit-agent.service.in" > "$UNITS/helix-polkit-agent.service"
    systemctl --user enable helix-polkit-agent.service
    return 0
  fi
  systemctl --user disable helix-polkit-agent.service 2>/dev/null || true
  systemctl --user stop helix-polkit-agent.service 2>/dev/null || true
  rm -f "$UNITS/helix-polkit-agent.service"
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
  STRUT=$(pick_bin helix-shell-strut) || {
    echo "build it first: (cargo build --release -p helix-shell-strut)" >&2
    exit 1
  }
  mkdir -p "$UNITS" "$HOME/.local/bin"
  install -m 0755 "$ROOT/tools/helix-panel" "$HOME/.local/bin/helix-panel"
  install -m 0755 "$ROOT/tools/helix-askpass" "$HOME/.local/bin/helix-askpass"
  ln -sf "$ROOT/tools/install-inference.sh" "$HOME/.local/bin/helix-install-inference"
  mkdir -p "$HOME/.config/environment.d"
  printf 'SUDO_ASKPASS=%s\n' "$HOME/.local/bin/helix-askpass" > "$HOME/.config/environment.d/helix-sudo.conf"
  ln -sf "$STRUT" "$HOME/.local/bin/helix-shell-strut"
  link_app helix-settings org.helix.Settings.desktop || true
  if [ -L "$HOME/.local/bin/helix-settings" ] || [ -x "$HOME/.local/bin/helix-settings" ]; then
    ln -sf "$(readlink -f "$HOME/.local/bin/helix-settings" 2>/dev/null || echo "$HOME/.local/bin/helix-settings")" \
      "$HOME/.local/bin/helix-settings-new" 2>/dev/null || true
  fi
  link_app helix-software org.helix.Software.desktop
  link_app helix-system-monitor org.helix.SystemMonitor.desktop
  link_app helix-mobius org.helix.Mobius.desktop || true
  link_app helix-models org.helix.Models.desktop || true
  # `helix`: Helix's actions, their history and Undo from a terminal or a script.
  link_app helix "" || true
  # The agent launcher, and its short names (helix-claude, helix-opencode, helix-codex, ha).
  link_app helix-agents "" || true
  [ -x "$HOME/.local/bin/helix-agents" ] && "$HOME/.local/bin/helix-agents" install-links >/dev/null 2>&1 || true
  # The lock screen and the agent display's input helper are looked up by name on PATH.
  link_app helix-lock "" || true
  link_app helix-agent-input "" || true
  link_app helix-identify "" || true
  # The old name is gone; drop a link left by an earlier install.
  rm -f "$HOME/.local/bin/helix-chat"
  escaped=$(printf '%s' "$BIN" | sed 's/ /\\\\x20/g')
  strut_escaped=$(printf '%s' "$STRUT" | sed 's/ /\\\\x20/g')
  # Legacy monolith unit (kept for nested tests / rollback).
  sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-tauri.service.in" > "$UNITS/helix-shell-tauri.service"
  sed -e "s|@SHELL_BIN@|$escaped|" -e 's|@ASKPASS@|%h/.local/bin/helix-askpass|' "$ROOT/tools/systemd/helix-shell-bar.service.in" > "$UNITS/helix-shell-bar.service"
  sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-panels.service.in" > "$UNITS/helix-shell-panels.service"
  sed "s|@STRUT_BIN@|$strut_escaped|" "$ROOT/tools/systemd/helix-shell-strut.service.in" > "$UNITS/helix-shell-strut.service"
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
  if NIGHT=$(pick_bin helix-nightlight); then
    night_escaped=$(printf '%s' "$NIGHT" | sed 's/ /\\\\x20/g')
    sed "s|@NIGHTLIGHT_BIN@|$night_escaped|" "$ROOT/tools/systemd/helix-nightlight.service.in" > "$UNITS/helix-nightlight.service"
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
  # Prefer strut + bar + panels; leave monolith unit installed but disabled when split is enabled.
  systemctl --user enable helix-shell-strut.service helix-shell-bar.service helix-shell-panels.service
  systemctl --user disable helix-shell-tauri.service 2>/dev/null || true
  if [ -f "$UNITS/helix-idle.service" ]; then
    systemctl --user enable helix-idle.service
  fi
  if [ -f "$UNITS/helix-nightlight.service" ]; then
    systemctl --user enable helix-nightlight.service
  fi
  if [ -f "$UNITS/helix-wallpaper.service" ]; then
    systemctl --user enable helix-wallpaper.service
  fi
  write_polkit_agent_unit || true
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
      echo "Bar bounce (strut takes over exclusive zone): $0 restart-bar"
      echo "Strut bounce (flaps work area — avoid with Cursor open): $0 restart-strut"
      echo "Full bar+panels bounce: $0 restart"
      # Do not start strut while a live bar may still reserve in-process — that would stack two zones.
      if systemctl --user is-active --quiet helix-shell-strut.service; then
        echo "Strut already active."
      else
        echo "Strut unit installed; it starts on the next restart-bar (avoids a double exclusive zone)."
      fi
      if [ -f "$UNITS/helix-polkit-agent.service" ]; then
        systemctl --user start helix-polkit-agent.service 2>/dev/null || true
      fi
    else
      systemctl --user start helix-shell-strut.service helix-shell-bar.service helix-shell-panels.service
      if [ -f "$UNITS/helix-idle.service" ]; then
        systemctl --user start helix-idle.service 2>/dev/null || true
      fi
      if [ -f "$UNITS/helix-nightlight.service" ]; then
        systemctl --user start helix-nightlight.service 2>/dev/null || true
      fi
      if [ -f "$UNITS/helix-wallpaper.service" ]; then
        systemctl --user start helix-wallpaper.service 2>/dev/null || true
      fi
      if [ -f "$UNITS/helix-polkit-agent.service" ]; then
        systemctl --user start helix-polkit-agent.service 2>/dev/null || true
      fi
      echo "Helix Shell started (strut + bar + panels)."
    fi
    ;;
  restart-panels)
    write_units_and_links
    echo "Restarting helix-shell-panels only (strut + bar stay up)…"
    systemctl --user restart helix-shell-panels.service
    echo "Panels restarted."
    ;;
  restart-bar)
    write_units_and_links
    echo "Restarting helix-shell-bar (strut + panels stay mapped)…"
    systemctl --user stop helix-shell-tauri.service 2>/dev/null || true
    systemctl --user start helix-shell-strut.service
    # Panels no longer BindsTo the bar — leave them running across bar bounce.
    systemctl --user restart helix-shell-bar.service
    systemctl --user start helix-shell-panels.service
    echo "Bar restarted; strut and panels left running."
    ;;
  restart-strut)
    write_units_and_links
    echo "Restarting helix-shell-strut (work area will flap — can disrupt Cursor)…"
    systemctl --user restart helix-shell-strut.service
    echo "Strut restarted."
    ;;
  restart)
    write_units_and_links
    echo "Restarting shell bar+panels (strut stays)…"
    systemctl --user stop helix-shell-tauri.service 2>/dev/null || true
    systemctl --user start helix-shell-strut.service
    # Restart separately so a momentary bar inactive state cannot cascade (Upholds removed,
    # but restarting both at once still tears Overlay while Cursor is open).
    systemctl --user restart helix-shell-bar.service
    systemctl --user restart helix-shell-panels.service
    if [ -f "$UNITS/helix-idle.service" ]; then
      systemctl --user restart helix-idle.service
    fi
    if [ -f "$UNITS/helix-nightlight.service" ]; then
      systemctl --user restart helix-nightlight.service
    fi
    if [ -f "$UNITS/helix-wallpaper.service" ]; then
      systemctl --user restart helix-wallpaper.service
    fi
    echo "Restarted (strut left running). Prefer restart-bar alone while Cursor is open."
    ;;
  native)
    echo "The C++ session shell is retired. Use: $0 install" >&2
    exit 2
    ;;
  status)
    for unit in helix-shell-strut helix-shell-bar helix-shell-panels helix-shell-tauri helix-wallpaper helix-idle helix-nightlight helix-service; do
      printf '%-22s %s / %s\n' "$unit" "$(systemctl --user is-active $unit.service 2>/dev/null || true)" "$(systemctl --user is-enabled $unit.service 2>/dev/null || echo disabled)"
    done
    if systemctl --user is-active --quiet helix-shell-bar.service 2>/dev/null \
      && journalctl --user -u helix-shell-bar.service -b -o cat --no-pager 2>/dev/null | grep -q 'POLKIT agent registered'; then
      echo "polkit agent          native (helix-shell-bar)"
      if [ ! -S /run/polkit/agent-helper.socket ] 2>/dev/null; then
        echo "polkit helper         missing (sudo systemctl enable --now polkit-agent-helper.socket)" >&2
      fi
    elif systemctl --user is-active --quiet helix-polkit-agent.service 2>/dev/null; then
      echo "polkit agent          helix-polkit-agent (external binary)"
    elif systemctl --user is-active --quiet plasma-polkit-agent.service 2>/dev/null \
      || systemctl --user is-active --quiet polkit-kde-authentication-agent-1.service 2>/dev/null; then
      echo "polkit agent          user unit active"
    else
      if find_polkit_agent >/dev/null; then
        echo "polkit agent          not running (run: $0 install)"
      else
        echo "polkit agent          not installed (dnf install polkit-kde; helix-askpass covers sudo only)"
      fi
    fi
    if systemctl --user is-enabled helix-shell-dev.service >/dev/null 2>&1; then
      echo "helix-shell-dev        still enabled (legacy — run: $0 install)" >&2
    fi
    ;;
  *)
    echo "usage: $0 [install|restart-panels|restart-bar|restart-strut|restart|status]" >&2
    echo "  install         update links/units; start shell only if not already running" >&2
    echo "  restart-panels  bounce Overlay panels only (safe with Cursor open)" >&2
    echo "  restart-bar     bounce painted bar; strut stays (safer for Cursor)" >&2
    echo "  restart-strut   bounce exclusive-zone holder (flaps work area — avoid with Cursor)" >&2
    echo "  restart         bounce bar+panels; strut stays" >&2
    exit 2
    ;;
esac
