#!/bin/bash
# Notifications against the real shell, with a stand-in application and a real pointer: toasts come and go on time, the shade
# buttons send back the key the application offered, dismissing and Do not disturb behave, and the application hears about each close.
#   HELIX_NESTED_SCRIPT=tools/nested/notifications-test.sh tools/nested/run.sh notifications tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log
SIGNALS=$SCRATCH/notify-signals.log
FAILED=0
LISTENER=""
cleanup() { [ -n "$LISTENER" ] && kill "$LISTENER" 2>/dev/null; true; }
trap cleanup EXIT
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
probe() { python3 "$ROOT/tools/nested/notify-probe.py" "$@"; }
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_log() {
  for _ in $(seq 1 "${2:-60}"); do
    grep 'NOTIFICATIONS view' "$LOG" 2>/dev/null | tail -1 | grep -Eq "$1" && return 0
    sleep 0.25
  done
  echo "TIMEOUT waiting for view: $1 (newest: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1))" >&2
  return 1
}
wait_signal() {
  for _ in $(seq 1 "${2:-60}"); do grep -qx "$1" "$SIGNALS" 2>/dev/null && return 0; sleep 0.25; done
  echo "TIMEOUT waiting for signal: $1 (have: $(tr '\n' '|' < "$SIGNALS"))" >&2
  return 1
}
# Not used mid-test: niri itself posts a "screenshot captured" notification, which would change the counts.
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }

for _ in $(seq 1 80); do grep -q 'NOTIFICATIONS serving' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS serving' "$LOG" || { echo "FAIL: the shell did not take org.freedesktop.Notifications"; exit 1; }
probe listen >"$SIGNALS" 2>&1 &
LISTENER=$!
sleep 0.5

echo "--- 1. A notification is a toast, counted as unread, and ends on time"
ID1=$(probe send Mail $'Hello ‮ there' '<b>Bold</b> body' 1 -1 ok Reply)
wait_log 'toasts=1 items=1 unread=1 dnd=false' || fail "toast not shown"
wait_log 'toasts=0 items=1 unread=1' 60 || fail "toast did not end"
wait_signal "NotificationClosed $ID1 1" 20 || fail "the application was not told it expired"

echo "--- 2. Opening the shade reads everything; a button sends back the application's own key"
click_id notifbtn
wait_log 'unread=0' || fail "opening the shade did not mark it read"
for _ in $(seq 1 20); do has_id notif-0-dismiss && break; sleep 0.25; done
has_id notif-0-dismiss || fail "shade rows missing"
# The notification expired (its buttons only work while it is live), so it has none.
has_id notif-0-action-0 && fail "an expired notification still offers buttons"
click_id notif-0-dismiss
wait_signal "NotificationClosed $ID1 2" 20 || fail "dismissal not reported to the application"
wait_log 'items=0' || fail "dismissed notification still listed"

echo "--- 3. A live notification's button sends the application's key, then closes it"
ID2=$(probe send Chat "Question" "Reply now?" 1 0 yes Yes no No)
wait_log 'items=1' || fail "second notification missing"
for _ in $(seq 1 20); do has_id notif-0-action-1 && break; sleep 0.25; done
has_id notif-0-action-1 || fail "action buttons missing"
click_id notif-0-action-1
wait_signal "ActionInvoked $ID2 no" 20 || fail "the key the application offered was not sent back"
wait_signal "NotificationClosed $ID2 2" 20 || fail "close after the action not reported"

echo "--- 4. Do not disturb hides ordinary toasts but not critical ones"
click_id notif-dnd
wait_log 'dnd=true' || fail "do not disturb not on"
ID3=$(probe send Chat "quiet" "" 1 -1)
wait_log 'toasts=0 items=1 unread=[0-9]+ dnd=true' || fail "an ordinary toast showed during do not disturb"
ID4=$(probe send Power "Battery low" "3%" 2 -1)
wait_log 'toasts=1 items=2' || fail "a critical toast was hidden"

echo "--- 5. The application can withdraw its own notification"
probe close "$ID4"
wait_signal "NotificationClosed $ID4 3" 20 || fail "withdrawal not reported with reason 3"

echo "--- 6. Clear all"
click_id notif-clear
wait_log 'items=0' || fail "clear all left items"
click_id notif-dnd
wait_log 'dnd=false' || fail "do not disturb did not turn off"

if [ "$FAILED" -ne 0 ]; then echo FAILED; exit 1; fi
echo "ALL ok"
