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
    if ! grep -q 'helix-panel' "$NIRI"; then
      cp -p "$NIRI" "$NIRI.bak-helix-$(date +%Y%m%d-%H%M%S)"
      python3 - "$NIRI" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
P = '"/home/' + __import__('os').environ.get('USER', '') + '/.local/bin/helix-panel"'
swaps = {
  'spawn "noctalia" "msg" "panel-toggle" "launcher"': f'spawn {P} "launcher"',
  'spawn "noctalia" "msg" "panel-toggle" "control-center"': f'spawn {P} "control-center"',
  'spawn "noctalia" "msg" "settings-toggle"': f'spawn {P} "settings"',
  'spawn "noctalia" "msg" "window-switcher"': f'spawn {P} "window-switcher"',
  'spawn "noctalia" "msg" "volume-up"': f'spawn {P} "volume-up"',
  'spawn "noctalia" "msg" "volume-down"': f'spawn {P} "volume-down"',
  'spawn "noctalia" "msg" "volume-mute"': f'spawn {P} "volume-mute"',
  'spawn "noctalia" "msg" "mic-mute"': f'spawn {P} "mic-mute"',
  'spawn "noctalia" "msg" "brightness-up"': f'spawn {P} "brightness-up"',
  'spawn "noctalia" "msg" "brightness-down"': f'spawn {P} "brightness-down"',
}
for old, new in swaps.items():
    text = text.replace(old, new)
open(path, 'w').write(text)
PY
      echo "key binds now go through helix-panel (backup: $NIRI.bak-helix-*)"
    fi
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
