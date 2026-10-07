#!/bin/bash
# Like tauri-shell.sh, but the desktop service is tools/nested/fake-desktop.py: scripted Wi-Fi networks, nothing real behind it.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export HELIX_SURFACE_CONTROL="${XDG_STATE_HOME}/surface-control"
# A fake battery, and the file where the page reports where its controls are (tests click controls by name).
mkdir -p "${XDG_STATE_HOME}/power_supply/BAT0"
printf 'Battery\n' > "${XDG_STATE_HOME}/power_supply/BAT0/type"; printf '64\n' > "${XDG_STATE_HOME}/power_supply/BAT0/capacity"; printf 'Discharging\n' > "${XDG_STATE_HOME}/power_supply/BAT0/status"
export HELIX_POWER_SUPPLY_DIR="${XDG_STATE_HOME}/power_supply" HELIX_UI_LAYOUT_LOG="${XDG_STATE_HOME}/layout.json"
python3 "$ROOT/tools/nested/fake-desktop.py" "${XDG_STATE_HOME}" > "${XDG_STATE_HOME}/fake-desktop.log" 2>&1 &
echo $! > "${XDG_STATE_HOME}/helix-service.pid"   # run.sh stops it by pid, like the real service
for _ in $(seq 1 50); do grep -q "ready" "${XDG_STATE_HOME}/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "${XDG_STATE_HOME}/shell.pid"
exec "$ROOT/rust/target/release/helix-shell-tauri"
