#!/bin/bash
# HELIX_NESTED_SCRIPT: the control center's Bluetooth section against tools/nested/fake-desktop.py (no real Bluetooth is reachable).
#   HELIX_NESTED_SCRIPT=tools/nested/bluetooth-test.sh tools/nested/run.sh bt tools/nested/tauri-fake-desktop.sh
# Clicks every control with a real pointer, then checks the exact list of actions the service received.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
ACTIONS=$SCRATCH/state/fake-bluetooth-actions.log
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
click() { "$PT" "$1" "$2" click; sleep 0.9; }
# Positions measured from a screenshot of this layout (x as a fraction of the width, y rows in px of a 1406 px tall screen).
X_NAME=0.8638; X_FORGET=0.965; X_SCAN=0.9188; X_TOGGLE=0.9619
y() { python3 -c "print(round($1/1406,4))"; }
HEAD=$(y 613); R0=$(y 700); R1=$(y 748); R2=$(y 796); R3=$(y 844)
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2; shot bt-open
echo "--- 1. a device that is not paired sends nothing"
click $X_NAME $R3; [ ! -s "$ACTIONS" ] && echo "ok: nothing sent for the unpaired speaker" || echo "FAIL: something was sent"
echo "--- 2. clicking a connected device disconnects it"; click $X_NAME $R0
echo "--- 3. clicking a paired idle device connects it (Phone is third alphabetically)"; click $X_NAME $R2; shot bt-phone
echo "--- 4. the other one too (Phone moved up; Earbuds is now second)"; click $X_NAME $R1
echo "--- 5. Forget removes a paired device (Earbuds sorts first of the two connected)"; click $X_FORGET $R0; shot bt-forgot
echo "--- 6. Scan starts, then Stop stops"; click $X_SCAN $HEAD; click $X_SCAN $HEAD
echo "--- 7. Bluetooth off, then on"; click $X_TOGGLE $HEAD; shot bt-off; click $X_TOGGLE $HEAD
wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: Escape closed the panel"
EXPECTED='ACTION disconnect {"path": "/d/buds"}
ACTION connect {"path": "/d/phone"}
ACTION connect {"path": "/d/buds"}
ACTION forget {"path": "/d/buds"}
ACTION scan {}
ACTION stop-scan {}
ACTION enable {"value": false}
ACTION enable {"value": true}'
echo "=== actions the service received, in order:"; cat "$ACTIONS"
if [ "$(grep '^ACTION' "$ACTIONS")" = "$EXPECTED" ]; then echo "PASS: exactly the expected actions, in order (nothing for the unpaired device)"; else echo "FAIL: the actions differ from what was expected"; fi
echo "refused by the service (the shell must never send these): $(grep -c '^REFUSED' "$ACTIONS" 2>/dev/null || true)"
