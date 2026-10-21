#!/bin/bash
# The shell honours the notification settings file Settings writes: how long a banner stays when the application did not say, and how many
# show at once, with no restart. An application that asks for a time keeps it.
#   HELIX_NESTED_SCRIPT=tools/nested/notifications-prefs-test.sh tools/nested/run.sh notifications-prefs tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log
FAILED=0
FILE=$SCRATCH/config/helix/notification-settings.json
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
for _ in $(seq 1 80); do grep -q 'NOTIFICATIONS serving' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS serving' "$LOG" || { echo "FAIL: the shell did not take org.freedesktop.Notifications"; exit 1; }
echo "--- 1. a short banner time: the toast is gone after about two seconds, not six"
mkdir -p "$(dirname "$FILE")"
echo '{"version":1,"toast_seconds":2,"max_toasts":1}' > "$FILE"
for _ in $(seq 1 20); do grep -q 'NOTIFICATIONS settings seconds=2 toasts=1' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS settings seconds=2 toasts=1' "$LOG" && echo "ok: the shell noticed the change" || fail "settings not noticed"
probe send Mail "Hello" "body" 1 -1 >/dev/null
wait_log 'toasts=1 items=1' 20 && echo "ok: shown" || fail "toast not shown"
wait_log 'toasts=0 items=1' 16 && echo "ok: ended on the chosen time" || fail "toast outlived its chosen time"
echo "--- 2. one banner at once: the others wait in the list"
probe send Chat "One" "x" 1 -1 >/dev/null
probe send Chat2 "Two" "x" 1 -1 >/dev/null
probe send Chat3 "Three" "x" 1 -1 >/dev/null
wait_log 'toasts=1 items=4' 20 && echo "ok: one banner, all four in the list" || fail "wrong banner count: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1)"
echo "--- 2b. do not disturb from the file hides an ordinary banner"
echo '{"version":1,"toast_seconds":6,"max_toasts":3,"dnd":true}' > "$FILE"
for _ in $(seq 1 20); do grep -q 'NOTIFICATIONS settings seconds=6 toasts=3 dnd=true' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS settings seconds=6 toasts=3 dnd=true' "$LOG" && echo "ok: do not disturb from the file" || fail "dnd not applied"
probe send Quiet "hidden" "x" 1 -1 >/dev/null
wait_log 'toasts=0 .*dnd=true' 20 && echo "ok: an ordinary banner stayed hidden" || fail "banner showed during do not disturb: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1)"
echo "--- 3. back to the defaults, and a damaged file means the defaults"
echo 'not json' > "$FILE"
for _ in $(seq 1 20); do grep -q 'NOTIFICATIONS settings seconds=6 toasts=3 dnd=false' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS settings seconds=6 toasts=3 dnd=false' "$LOG" && echo "ok: defaults again" || fail "damaged file did not give the defaults"
echo "--- 4. quiet hours covering this hour hide an ordinary banner; an urgent one still shows"
sleep 7
HOUR=$(date +%-H)
END=$(( (10#$HOUR + 1) % 24 ))
printf '{"version":1,"toast_seconds":6,"max_toasts":3,"dnd":false,"quiet":true,"quiet_start":%s,"quiet_end":%s}\n' "$HOUR" "$END" > "$FILE"
for _ in $(seq 1 20); do grep -q 'dnd=false quiet=true' "$LOG" && break; sleep 0.25; done
grep -q 'dnd=false quiet=true' "$LOG" && echo "ok: quiet hours cover this hour" || fail "quiet hours not applied: $(grep 'NOTIFICATIONS settings' "$LOG" | tail -2)"
ITEMS=$(grep 'NOTIFICATIONS view' "$LOG" | tail -1 | sed -n 's/.*items=\([0-9]*\).*/\1/p')
probe send Day "hidden" "x" 1 -1 >/dev/null
wait_log "toasts=0 items=$((ITEMS + 1))" 20 && echo "ok: an ordinary banner stayed hidden during quiet hours" || fail "banner showed during quiet hours: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1)"
probe send Power "low" "x" 2 -1 >/dev/null
wait_log "toasts=1 items=$((ITEMS + 2))" 20 && echo "ok: an urgent banner still shows" || fail "urgent banner hidden: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1)"
echo "--- 5. a window that does not cover this hour leaves ordinary banners showing"
sleep 7
START=$(( (10#$HOUR + 2) % 24 ))
END=$(( (10#$HOUR + 3) % 24 ))
OUTSIDE=$(grep -c 'dnd=false quiet=false' "$LOG" || true)
printf '{"version":1,"toast_seconds":6,"max_toasts":3,"dnd":false,"quiet":true,"quiet_start":%s,"quiet_end":%s}\n' "$START" "$END" > "$FILE"
NOW=$OUTSIDE
for _ in $(seq 1 20); do NOW=$(grep -c 'dnd=false quiet=false' "$LOG" || true); [ "$NOW" -gt "$OUTSIDE" ] && break; sleep 0.25; done
[ "$NOW" -gt "$OUTSIDE" ] && echo "ok: quiet hours are outside this hour" || fail "quiet hours stayed on: $(grep 'NOTIFICATIONS settings' "$LOG" | tail -2)"
probe send Outside "shown" "x" 1 -1 >/dev/null
wait_log "toasts=2 items=$((ITEMS + 3))" 20 && echo "ok: an ordinary banner shows outside quiet hours" || fail "banner stayed hidden: $(grep 'NOTIFICATIONS view' "$LOG" | tail -1)"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
