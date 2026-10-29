#!/bin/bash
# Notification summaries (docs/design/north-star.md, "intelligence in place"): two notifications from one app are one stack in the shade, and
# opening the shade has the (scripted) Summaries model write the stack's line; a click opens the stack to its notifications, and they can be
# cleared together. One notification from another app stays a row of its own.   tools/nested/ui-suite.sh notification-summaries
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
LOG=$SCRATCH/program.log
source "$ROOT/tools/nested/ui-lib.sh"; LAYOUT=$SCRATCH/state/layout.json
probe() { python3 "$ROOT/tools/nested/notify-probe.py" "$@"; }
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { sleep 2.5; niri msg action screenshot-screen >/dev/null 2>&1; sleep 2; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 80); do grep -q 'NOTIFICATIONS serving' "$LOG" && break; sleep 0.25; done
wait_for "LIVE Sound" 1 120 || exit 1

echo "--- 1. two from Chat are one stack, summarised when the shade opens"
probe send Chat "Pat" "Are we still deploying on Friday?" 1 -1 >/dev/null
probe send Chat "Sam" "Friday works for me, after lunch" 1 -1 >/dev/null
probe send Mail "Invoice 42" "Your invoice is ready" 1 -1 >/dev/null
for _ in $(seq 1 40); do grep 'NOTIFICATIONS view' "$LOG" | tail -1 | grep -q "items=[3-9]" && break; sleep 0.25; done
click_id notifbtn
wait_for "NOTIFICATIONS summary Chat" 1 60 && echo "ok: $(grep -o 'NOTIFICATIONS summary Chat.*' "$LOG" | tail -1)" || fail "no summary: $(grep 'summary' "$LOG" | tail -2)"
grep -q "NOTIFICATIONS summary Mail" "$LOG" && fail "a single notification was summarised"
for _ in $(seq 1 20); do has_id notif-summary-Chat && break; sleep 0.25; done
has_id notif-summary-Chat && echo "ok: the Chat stack shows its summary" || fail "no summary on the stack"
has_id notif-0-dismiss && echo "ok: Mail's one notification is a row of its own" || fail "the single notification is not a row"
has_id notif-stack-Chat-less && fail "the Chat stack is open before it is clicked"
has_id notif-summary-Mail && fail "a single notification has a stack"
shot summaries   # (niri adds a "screenshot" notification of its own: positions shift, names do not)

echo "--- 2. a click opens the stack; the stack can be cleared"
items() { grep 'NOTIFICATIONS view' "$LOG" | tail -1 | grep -o 'items=[0-9]*' | cut -d= -f2; }
click_id notif-stack-Chat
for _ in $(seq 1 20); do has_id notif-stack-Chat-less && break; sleep 0.25; done
has_id notif-stack-Chat-less && ! has_id notif-stack-Chat && echo "ok: the stack opened to its notifications" || fail "the stack did not open"
shot stack-open
BEFORE=$(items)
click_id notif-stack-Chat-clear
for _ in $(seq 1 40); do [ "$(items)" -le $((BEFORE - 2)) ] && break; sleep 0.25; done
[ "$(items)" -le $((BEFORE - 2)) ] && echo "ok: clearing the stack took both ($BEFORE → $(items))" || fail "after clearing: $(items) of $BEFORE"
grep -q "panicked" "$LOG" && fail "the shell panicked" || true
[ "$FAILED" = 0 ] && echo "PASS: notification-summaries" || exit 1
