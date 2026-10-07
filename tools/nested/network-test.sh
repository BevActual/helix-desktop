#!/bin/bash
# HELIX_NESTED_SCRIPT: the control center's Wi-Fi section against tools/nested/fake-desktop.py (no real network is reachable).
#   HELIX_NESTED_SCRIPT=tools/nested/network-test.sh tools/nested/run.sh net tools/nested/tauri-fake-desktop.sh
# Clicks through every control with a real pointer and then checks the exact list of actions the service received: what must have been
# sent, and (as important) what must never have been sent.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
ACTIONS=$SCRATCH/state/fake-network-actions.log
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
source "$ROOT/tools/nested/ui-lib.sh"
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2
click_id tile-wifi-open; shot net-open
count() { [ -f "$ACTIONS" ] && grep -c "^ACTION" "$ACTIONS" || echo 0; }
echo "--- 1. networks that cannot be joined here send nothing"
N0=$(count); click_id "net:Neighbour"; click_id "net:Corp"; echo "actions sent for Neighbour (needs a password) and Corp (enterprise): $(( $(count) - N0 ))"
echo "--- 2. an open network is joined by clicking it"; click_id "net:Cafe"; shot net-joined-cafe
echo "--- 3. Disconnect"; click_id wifi-disconnect
echo "--- 4. a saved secured network is joined without a password"; click_id "net:Home WiFi"; shot net-joined-home
echo "--- 5. Forget"; click_id "net-forget:Home WiFi"; shot net-forgot
echo "--- 6. the VPN"; click_id "vpn:Work VPN"
echo "--- 7. Wi-Fi off, then on, then Scan"; click_id wifi-toggle; shot net-off; click_id wifi-toggle; click_id wifi-scan
echo "--- 8. Escape goes back, then closes"
wtype -k Escape; sleep 1; wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: closed"
EXPECTED='ACTION connect {"path": "/ap/cafe"}
ACTION disconnect {}
ACTION connect {"path": "/ap/home"}
ACTION forget {"ssid": "Home WiFi"}
ACTION vpn-connect {"path": "/vpn/work"}
ACTION enable {"value": false}
ACTION enable {"value": true}
ACTION scan {}'
echo "=== actions the service received, in order:"; cat "$ACTIONS"
if [ "$(grep '^ACTION' "$ACTIONS")" = "$EXPECTED" ]; then echo "PASS: exactly the expected actions, in order (nothing for the networks that need a password or are enterprise)"; else echo "FAIL: the actions differ from what was expected"; fi
echo "refused by the service (the shell must never send these): $(grep -c '^REFUSED' "$ACTIONS" 2>/dev/null || true)"
