#!/bin/bash
# Helix Settings against a scripted desktop service (location settings) and a scripted systemd-timedated on a private "system" bus.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/target/fast/helix-settings" "$ROOT/target/release/helix-settings" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
BUS=$(dbus-daemon --config-file="$ROOT/tools/nested/bus.conf" --fork --print-address --print-pid 2>/dev/null)
ADDRESS=$(echo "$BUS" | sed -n 1p)
[ -n "$ADDRESS" ] || { echo "no private bus"; exit 1; }
echo "$BUS" | sed -n 2p > "$XDG_STATE_HOME/private-bus.pid"
python3 "$ROOT/tools/nested/fake-timedate.py" "$XDG_STATE_HOME" "$ADDRESS" > "$XDG_STATE_HOME/fake-timedate.out" 2>&1 &
for _ in $(seq 1 50); do grep -q ready "$XDG_STATE_HOME/fake-timedate.out" 2>/dev/null && break; sleep 0.1; done
export DBUS_SYSTEM_BUS_ADDRESS="$ADDRESS"
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
