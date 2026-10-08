#!/bin/bash
# The system tray against a stand-in StatusNotifierItem application: it registers with the watcher this shell serves, its icon appears
# in the bar, clicks reach it, it can hide itself, it disappears when it leaves, and a bad registration is refused.
#   HELIX_NESTED_SCRIPT=tools/nested/tray-test.sh tools/nested/run.sh tray tools/nested/tauri-fake-desktop.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json; export TRAY_LOG=$SCRATCH/tray-app.log
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_tray() { for _ in $(seq 1 "${2:-40}"); do grep 'TRAY view' "$LOG" | tail -1 | grep -Eq "$1" && return 0; sleep 0.25; done; echo "TIMEOUT waiting for: $1 (newest: $(grep 'TRAY view' "$LOG" | tail -1))" >&2; return 1; }
wait_app() { for _ in $(seq 1 40); do grep -q "$1" "$TRAY_LOG" 2>/dev/null && return 0; sleep 0.25; done; echo "TIMEOUT waiting for app call: $1 (have: $(tr '\n' '|' < "$TRAY_LOG" 2>/dev/null))" >&2; return 1; }
click_tray() { local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" tray-0 0.5) || { fail "no tray-0"; return 1; }; "$PT" $pos $1; sleep 0.8; }
for _ in $(seq 1 120); do grep -q "TRAY serving" "$LOG" && break; sleep 0.25; done
grep -q "TRAY serving" "$LOG" || { echo "FAIL: the shell did not become the tray watcher"; exit 1; }
rm -f "$TRAY_LOG"; export TRAY_CMD=$SCRATCH/tray.cmd; : > "$TRAY_CMD"
python3 "$ROOT/tools/nested/tray-app.py" 2>/dev/null & APP=$!
trap 'kill $APP 2>/dev/null' EXIT
cmd() { echo "$1" >> "$TRAY_CMD"; }

echo "--- 1. the application registers and its icon appears"
wait_tray 'items=1' || fail "no tray item"
for _ in $(seq 1 20); do has_id tray-0 && break; sleep 0.25; done
has_id tray-0 && echo "ok: icon in the bar" || fail "no icon in the bar"
sleep 0.6

echo "--- 2. clicks reach the application"
click_tray click;  wait_app "Activate" && echo "ok: Activate" || fail "left click"
click_tray rclick; wait_app "ContextMenu" && echo "ok: ContextMenu" || fail "right click"
click_tray mclick; wait_app "SecondaryActivate" && echo "ok: SecondaryActivate" || fail "middle click"

echo "--- 3. a change in the application updates the bar; passive hides it"
before=$(grep -c 'TRAY view' "$LOG")
cmd "title Something new"; sleep 1.2
[ "$(grep -c 'TRAY view' "$LOG")" -gt "$before" ] && echo "ok: re-read after NewTitle" || fail "no refresh after a title change"
cmd "status Passive"; wait_tray 'items=0' && echo "ok: hidden while passive" || fail "passive item still shown"
cmd "status Active"; wait_tray 'items=1' && echo "ok: shown again" || fail "active item not shown again"

echo "--- 4. a registration that is not a bus name and path is refused"
if gdbus call --session --dest org.kde.StatusNotifierWatcher --object-path /StatusNotifierWatcher --method org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem "garbage name with spaces" 2>&1 | grep -qi 'invalid\|error'; then echo "ok: refused"; else fail "a bad registration was accepted"; fi
wait_tray 'items=1' 4 && echo "ok: nothing was added"

echo "--- 5. when the application leaves, its icon goes"
cmd quit; wait_tray 'items=0' && echo "ok: gone with the application" || fail "icon stayed after the application left"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
