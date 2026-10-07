#!/bin/bash
# Chooses which shell is the session shell. Changes only the person's own user units and niri key binds, never /usr/local.
#   tools/switch-shell.sh tauri     the Tauri shell takes over (stops the native shell, enabled at login)
#   tools/switch-shell.sh native    back to the native shell (disables the Tauri unit)
#   tools/switch-shell.sh status
# Key binds call ~/.local/bin/helix-panel, which works under either shell, so switching back needs no config edit.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UNITS=$HOME/.config/systemd/user
NIRI=$HOME/.config/niri/config.kdl
case "${1:-status}" in
  tauri)
    BIN=$ROOT/rust/target/release/helix-shell-tauri
    [ -x "$BIN" ] || { echo "build it first: (cd rust && cargo build --release)" >&2; exit 1; }
    mkdir -p "$UNITS" "$HOME/.local/bin"
    install -m 0755 "$ROOT/tools/helix-panel" "$HOME/.local/bin/helix-panel"
    # systemd reads \x20 for a space in ExecStart; systemd-escape --path would also turn / into -, so escape spaces only.
    # Doubled because the next sed would read \x20 in its replacement as a space again.
    escaped=$(printf '%s' "$BIN" | sed 's/ /\\\\x20/g')
    sed "s|@SHELL_BIN@|$escaped|" "$ROOT/tools/systemd/helix-shell-tauri.service.in" > "$UNITS/helix-shell-tauri.service"
    # Idempotent: only binds still pointing at the native shell are changed, and each file is backed up before its first change.
    python3 - "$HOME/.config/niri/config.kdl" "$HOME/.config/niri/noctalia-setup.kdl" "$HOME/.config/niri/noctalia-binds.kdl" <<'PY'
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
    old = "spawn " + " ".join('"%s"' % word for word in ["noctalia", "msg"] + native.split())
    swaps[old] = "spawn %s \"%s\"" % (panel, name)
    swaps['spawn-sh "noctalia msg ' + native + '"'] = "spawn %s \"%s\"" % (panel, name)
for path in sys.argv[1:]:
    text = open(path).read()
    new = text
    for old, replacement in swaps.items():
        new = new.replace(old, replacement)
    if new != text:
        shutil.copy2(path, "%s.bak-helix-%s" % (path, time.strftime("%Y%m%d-%H%M%S")))
        open(path, "w").write(new)
        print("key binds changed in", path)
PY
    systemctl --user daemon-reload
    systemctl --user stop helix-shell-tauri-trial.service 2>/dev/null || true
    systemctl --user disable helix-shell-dev.service 2>/dev/null || true
    systemctl --user enable helix-shell-tauri.service
    systemctl --user start helix-shell-tauri.service
    echo "The Tauri shell is the session shell."
    ;;
  native)
    systemctl --user disable helix-shell-tauri.service 2>/dev/null || true
    systemctl --user enable helix-shell-dev.service
    systemctl --user start helix-shell-dev.service
    echo "The native shell is the session shell."
    ;;
  status)
    for unit in helix-shell-dev helix-shell-tauri helix-service; do
      printf '%-22s %s / %s\n' "$unit" "$(systemctl --user is-active $unit.service || true)" "$(systemctl --user is-enabled $unit.service 2>/dev/null || echo disabled)"
    done
    ;;
  *) echo "usage: $0 tauri|native|status" >&2; exit 2 ;;
esac
