#!/bin/bash
# What the top bar shows follows ~/.config/helix/bar-settings.json (Settings > Top bar): hidden items leave the bar at once, the control center and
# notification buttons never can, and the seconds make the clock tick every second.
#   HELIX_NESTED_SCRIPT=tools/nested/shell-bar-prefs-test.sh tools/nested/run.sh bar-prefs tools/nested/tauri-shell.sh
SCRATCH=$1; LOG=$SCRATCH/program.log; LAYOUT=$SCRATCH/state/layout.json; FILE=$SCRATCH/config/helix/bar-settings.json; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
# Whether the bar's element with this name is drawn (reported with a size) right now.
shown() { python3 - "$LAYOUT" "$1" <<'PY'
import json, sys
e = json.load(open(sys.argv[1]))["elements"].get(sys.argv[2])
sys.exit(0 if e and e["w"] > 0 and e["h"] > 0 else 1)
PY
}
wait_state() { for _ in $(seq 1 40); do if shown "$1"; then [ "$2" = yes ] && return 0; else [ "$2" = no ] && return 0; fi; sleep 0.25; done; return 1; }
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done; sleep 2
wait_state apps yes && wait_state workspaces yes && wait_state clock yes && echo "ok: by default the bar shows everything" || fail "default bar: $(python3 -c 'import json;print(list(json.load(open("'$LAYOUT'"))["elements"]))')"
shot bar-default
mkdir -p "$(dirname "$FILE")"
echo '{"version":1,"show":{"apps":false,"workspaces":false,"mobius":false,"date":false,"battery_percent":false}}' > "$FILE"
wait_state apps no && wait_state ask no && wait_state workspaces no && echo "ok: the Apps button, the Mobius button and the workspaces left the bar" || fail "items still drawn"
shown clock && shown controlbtn && shown notifbtn && echo "ok: the clock, the control center and the notifications are still there" || fail "essentials gone"
grep -q "BAR prefs" "$LOG" && echo "ok: the shell logged the change" || fail "no log line"
# The date's own width: the clock is narrower without it.
python3 - "$LAYOUT" "$SCRATCH/clock-wide" <<'PY'
import json, sys
open(sys.argv[2], "w").write(str(json.load(open(sys.argv[1]))["elements"]["clock"]["w"]))
PY
python3 -c "import sys; sys.exit(0 if float(open(sys.argv[1]).read()) < 120 else 1)" "$SCRATCH/clock-wide" && echo "ok: the date is gone from the clock" || fail "clock still wide: $(cat "$SCRATCH/clock-wide")"
shot bar-hidden
# The seconds: the clock text changes within two seconds.
echo '{"version":1,"show":{"seconds":true}}' > "$FILE"
sleep 2
shown apps && echo "ok: choosing again brings back what was hidden before" || fail "apps not back"
shot bar-seconds
# The active window: off by default; on, the bar names the window that has the focus.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$ROOT/tools/nested/probe_window.py" >/dev/null 2>&1 &
PROBE=$!
sleep 3
shown bar-window && fail "the active window showed although it is off by default" || echo "ok: the active window is off by default"
echo '{"version":1,"show":{"window":true}}' > "$FILE"
wait_state bar-window yes && echo "ok: with it on, the bar names the focused window" || fail "bar-window not shown"
grep -q 'ACTIVE-WINDOW .*title="probe"' "$LOG" && echo "ok: the shell saw the window's title" || fail "no active window: $(grep ACTIVE-WINDOW "$LOG" | tail -2)"
sleep 2; shot bar-window
kill $PROBE 2>/dev/null
for _ in $(seq 1 20); do grep ACTIVE-WINDOW "$LOG" | tail -1 | grep -q 'title=""' && break; sleep 0.25; done
grep ACTIVE-WINDOW "$LOG" | tail -1 | grep -q 'title=""' && echo "ok: nothing focused, nothing named" || fail "stale name: $(grep ACTIVE-WINDOW "$LOG" | tail -1)"
[ "$FAILED" = 0 ] && echo "PASS: bar prefs" || exit 1
