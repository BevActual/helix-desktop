#!/bin/bash
# HELIX_NESTED_SCRIPT: the Android-style control center (header, tiles, details) against tools/nested/fake-desktop.py.
#   HELIX_NESTED_SCRIPT=tools/nested/quick-settings-test.sh tools/nested/run.sh qs tools/nested/tauri-fake-desktop.sh
# Controls are found by name from the layout the page reports, never by pixel. Checks what each tile sends, and navigation.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
source "$ROOT/tools/nested/ui-lib.sh"
S=$SCRATCH/state; FAILED=0
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
check() { if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1"; echo "  expected: $(echo "$3" | tr '\n' '|')"; echo "  got:      $(echo "$2" | tr '\n' '|')"; FAILED=1; fi; }
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2
for id in time battery tile-wifi tile-bt tile-dark tile-night tile-wifi-open tile-bt-open; do has_id $id && echo "ok: '$id' is on screen" || { echo "FAIL: '$id' missing"; FAILED=1; }; done
shot qs-main

echo "--- tiles that are plain settings"
click_id tile-dark; click_id tile-dark; click_id tile-night; click_id tile-night
check "dark theme off then on; night light on then off" "$(cat $S/fake-settings-actions.log 2>/dev/null)" 'UPDATE theme.mode "light"
UPDATE theme.mode "dark"
UPDATE nightlight.enabled true
UPDATE nightlight.enabled false'

echo "--- the Internet and Bluetooth tiles switch the radio; the arrow does not"
click_id tile-wifi; click_id tile-wifi; click_id tile-bt; click_id tile-bt
check "Wi-Fi off then on" "$(grep '^ACTION' $S/fake-network-actions.log 2>/dev/null)" 'ACTION enable {"value": false}
ACTION enable {"value": true}'
check "Bluetooth off then on" "$(grep '^ACTION' $S/fake-bluetooth-actions.log 2>/dev/null)" 'ACTION enable {"value": false}
ACTION enable {"value": true}'

echo "--- details open from the arrow, with a back button; Escape goes back before it closes"
click_id tile-wifi-open; has_id back && has_id wifi-toggle && ! has_id tile-dark && echo "ok: the Internet page replaced the tiles" || { echo "FAIL: the Internet page"; FAILED=1; }; shot qs-wifi
click_id back; has_id tile-dark && ! has_id back && echo "ok: back shows the tiles again" || { echo "FAIL: back"; FAILED=1; }
click_id tile-bt-open; has_id bt-toggle && echo "ok: the Bluetooth page opened" || { echo "FAIL: bluetooth page"; FAILED=1; }
wtype -k Escape; sleep 1; has_id tile-dark && echo "ok: Escape went back to the tiles" || { echo "FAIL: Escape in a detail page"; FAILED=1; }
[ "$(grep -c 'PANEL closed' "$LOG")" = 0 ] && echo "ok: ...and did not close the panel" || { echo "FAIL: the panel closed"; FAILED=1; }
wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: a second Escape closed the panel"
echo "--- opening it again starts at the tiles"
echo control-center >> "$CONTROL"; wait_for "PANEL open" 2; sleep 1.5; has_id tile-dark && echo "ok: back at the main view" || { echo "FAIL: reopened inside a detail page"; FAILED=1; }
[ $FAILED = 0 ] && echo "PASS: quick settings" || echo "FAIL: quick settings"
