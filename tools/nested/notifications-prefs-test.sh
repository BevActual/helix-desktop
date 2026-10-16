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
echo "--- 3. back to the defaults, and a damaged file means the defaults"
echo 'not json' > "$FILE"
for _ in $(seq 1 20); do grep -q 'NOTIFICATIONS settings seconds=6 toasts=3' "$LOG" && break; sleep 0.25; done
grep -q 'NOTIFICATIONS settings seconds=6 toasts=3' "$LOG" && echo "ok: defaults again" || fail "damaged file did not give the defaults"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
