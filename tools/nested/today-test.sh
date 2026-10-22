#!/bin/bash
# The Today panel behind the clock (the calendar alone; the bell opens the notification list alone): the month grid for today's real date, moving between months, and coming back. Clicks are real pointer
# clicks on named controls.
#   HELIX_NESTED_SCRIPT=tools/nested/today-test.sh tools/nested/run.sh today tools/nested/tauri-shell.sh
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 40); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 120); do grep -q "LIVE" "$LOG" 2>/dev/null && break; sleep 0.25; done; sleep 2
TODAY=$(date +%Y-%m-%d)
NEXT=$(python3 -c "import datetime as d; t=d.date.today(); y,m=(t.year+(t.month==12), t.month%12+1); print(f'{y:04d}-{m:02d}-01')")
PREV=$(python3 -c "import datetime as d; t=d.date.today(); y,m=(t.year-(t.month==1), (t.month-2)%12+1); print(f'{y:04d}-{m:02d}-15')")
echo "--- 1. the clock opens the panel on this month, with today in it"
click_id clock; sleep 1.2
wait_id "day-$TODAY" && echo "ok: today ($TODAY) is on the grid" || fail "today is missing"
has_id cal-today && fail "the Today button shows while on the current month" || echo "ok: no Today button on the current month"
shot today-open
has_id notif-clear && fail "the notification list shows in the calendar panel" || echo "ok: the calendar panel is the calendar alone (no notification list)"
echo "--- 2. next month"
click_id cal-next; sleep 0.8
wait_id "day-$NEXT" && echo "ok: next month's first day is shown ($NEXT)" || fail "next month missing"
wait_id cal-today && echo "ok: a Today button appeared" || fail "no Today button"
shot today-next
echo "--- 3. back to today, then the month before"
click_id cal-today; sleep 0.8
wait_id "day-$TODAY" && echo "ok: Today returns to this month" || fail "did not return"
click_id cal-prev; sleep 0.8
wait_id "day-$PREV" && echo "ok: previous month's middle is shown ($PREV)" || fail "previous month missing"
click_id cal-prev; click_id cal-next; sleep 0.5
echo "--- 4. closing and opening again starts on this month"
click_id cal-prev; sleep 0.5
wtype -k Escape; sleep 1; click_id clock; sleep 1.2
wait_id "day-$TODAY" && has_id cal-prev && echo "ok: reopened on the current month" || fail "reopened elsewhere"
has_id cal-today && fail "reopened on another month" || true
echo "--- 5. the bell opens the notification list, and only that"
wtype -k Escape; sleep 1; click_id notifbtn; sleep 1.2
wait_id notif-clear && echo "ok: the bell shows the notification list" || fail "no notification list from the bell"
has_id cal-prev && fail "the calendar shows in the notification panel" || echo "ok: and no calendar with it"
shot notifications-open
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
