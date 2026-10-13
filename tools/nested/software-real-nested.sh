#!/bin/bash
# Helix Software against the real Flatpak data of this machine (read-only: the test never clicks an action).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SOFTWARE_BIN:-$(ls -t "$ROOT/target/fast/helix-software" "$ROOT/target/release/helix-software" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
unset HELIX_SOFTWARE_FAKE
# Flatpak reads its system installation through the real system bus; the harness points that at nothing.
unset DBUS_SYSTEM_BUS_ADDRESS
export HOME=${HELIX_REAL_HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}
export XDG_DATA_HOME="$HOME/.local/share" XDG_CACHE_HOME="$HOME/.cache" XDG_CONFIG_HOME="$HOME/.config"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
