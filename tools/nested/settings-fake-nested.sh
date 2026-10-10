#!/bin/bash
# Helix Settings against tools/nested/fake-desktop.py (scripted Wi-Fi and Bluetooth), for the Network and Bluetooth pages.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/target/fast/helix-settings" "$ROOT/target/release/helix-settings" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
