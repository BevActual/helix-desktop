#!/bin/bash
# HELIX_NESTED_SCRIPT: the control center's Bluetooth section against tools/nested/fake-desktop.py (no real Bluetooth is reachable).
#   HELIX_NESTED_SCRIPT=tools/nested/bluetooth-test.sh tools/nested/run.sh bt tools/nested/tauri-fake-desktop.sh
# Clicks every control with a real pointer, then checks the exact list of actions the service received.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
ACTIONS=$SCRATCH/state/fake-bluetooth-actions.log
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
click() { "$PT" "$1" "$2" click; sleep 0.9; }
source "$ROOT/tools/nested/ui-lib.sh"
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2
click_id tile-bt-open; shot bt-open
echo "--- 1. a device that is not paired sends nothing"
click_id "bt:Kitchen Speaker"; [ ! -s "$ACTIONS" ] && echo "ok: nothing sent for the unpaired speaker" || echo "FAIL: something was sent"
echo "--- 2. clicking a connected device disconnects it"; click_id "bt:Earbuds"
echo "--- 3. clicking a paired idle device connects it"; click_id "bt:Phone"; shot bt-phone
echo "--- 4. the other one too"; click_id "bt:Earbuds"
echo "--- 5. Forget removes a paired device"; click_id "bt-forget:Earbuds"; shot bt-forgot
echo "--- 6. Scan starts, then Stop stops"; click_id bt-scan; click_id bt-scan
echo "--- 7. Bluetooth off, then on"; click_id bt-toggle; shot bt-off; click_id bt-toggle
wtype -k Escape; sleep 1; wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: Escape went back, then closed the panel"
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
