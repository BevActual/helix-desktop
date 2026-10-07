#!/bin/bash
# HELIX_NESTED_SCRIPT: the control center's Wi-Fi section against tools/nested/fake-desktop.py (no real network is reachable).
#   HELIX_NESTED_SCRIPT=tools/nested/network-test.sh tools/nested/run.sh net tools/nested/tauri-fake-desktop.sh
# Clicks through every control with a real pointer and then checks the exact list of actions the service received: what must have been
# sent, and (as important) what must never have been sent.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
ACTIONS=$SCRATCH/state/fake-network-actions.log
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
# Positions measured from a screenshot of this layout: x is a fraction of the width, rows start at y=316 px and are 48.7 px apart.
X_NAME=0.8638; X_SCAN=0.857; X_DISCONNECT=0.9063; X_TOGGLE=0.9619; X_FORGET=0.965
row_y() { python3 -c "print(round((316 + 48.7 * $1) / 1406, 4))"; }
HEAD_Y=0.1622; VPN_Y=$(python3 -c "print(round(517/1406,4))")
count() { [ -f "$ACTIONS" ] && grep -c "^ACTION" "$ACTIONS" || echo 0; }
click() { "$PT" "$1" "$2" click; sleep 0.9; }
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2; shot net-open
echo "--- 1. networks that cannot be joined here send nothing"
N0=$(count); click $X_NAME $(row_y 2); click $X_NAME $(row_y 3); echo "actions sent for Neighbour (needs a password) and Corp (enterprise): $(( $(count) - N0 ))"
echo "--- 2. an open network is joined by clicking it"
click $X_NAME $(row_y 1); shot net-joined-cafe
echo "--- 3. Disconnect"
click $X_DISCONNECT $HEAD_Y
echo "--- 4. a saved secured network is joined without a password"
click $X_NAME $(row_y 0); shot net-joined-home
echo "--- 5. Forget"
click $X_FORGET $(row_y 0); shot net-forgot
echo "--- 6. the VPN"
click $X_NAME $VPN_Y
echo "--- 7. Wi-Fi off, then on, then Scan"
# With Disconnect hidden (we are not connected) the header buttons sit further right.
click $X_TOGGLE $HEAD_Y; shot net-off; click $X_TOGGLE $HEAD_Y; click 0.9188 $HEAD_Y
echo "--- 8. Escape closes"
wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: closed"
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
