#!/bin/bash
# Mobius's task card: a request handed to the desktop assistant is shown where it was asked, step by step, and the person can stop it,
# review it, and see how it ended. Approving is not something Mobius can do: the test stands in for the shell by creating a file.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-act-test.sh tools/nested/run.sh mobius-act tools/nested/mobius-act-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
FAKE=$SCRATCH/state/fake-shell-act.log
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id composer && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
# Clicks the composer, types, and hands the text to the desktop assistant (the draft is cleared once it is handed over). The layout log is read after a pause so a
# card that has just appeared above the composer has already moved things.
say() { sleep 1; click_id composer; sleep 0.6; wtype "$1"; sleep 0.4; click_id act; sleep 0.5; }
wait_id() { for _ in $(seq 1 60); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
gone_id() { for _ in $(seq 1 40); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
echo "--- 1. nothing is shown before anything is handed over"
has_id act-task && fail "a task card with nothing to show" || echo "ok: no card on a quiet window"
echo "--- 2. handing a request over shows the task: working, then waiting for approval"
say "Make the screen dimmer"
wait_id act-task && echo "ok: a task card appeared" || fail "no task card"
shot act-working
wait_id act-task-review && echo "ok: it is waiting for approval, with Review" || fail "never asked for approval"
shot act-confirm
has_id act-task-stop && echo "ok: it can be cancelled" || fail "no cancel"
echo "--- 3. Review brings the shell's approval surface back; Mobius cannot approve"
click_id act-task-review; sleep 0.8
grep -q '^OPEN assistant' "$FAKE" && echo "ok: the assistant panel was asked for" || fail "review did nothing: $(cat "$FAKE")"
grep -q APPROVED "$FAKE" && fail "something approved without the person" || echo "ok: nothing was approved by Mobius"
echo "--- 4. approval in the shell moves it on to done"
touch "$SCRATCH/state/approve"
wait_id act-task-dismiss && echo "ok: it finished" || fail "never finished"
has_id act-task-failed && fail "a success was shown as a failure" || echo "ok: a success is not shown as a failure"
shot act-done
grep -q APPROVED "$FAKE" && echo "ok: approved in the shell" || fail "not approved"
echo "--- 5. dismiss clears the card"
click_id act-task-dismiss; gone_id act-task && echo "ok: the card is gone" || fail "the card stayed"
echo "--- 6. Stop cancels a task in progress"
say "Dim it again"
wait_id act-task-stop && click_id act-task-stop; gone_id act-task && echo "ok: stopped, the card went away" || fail "did not stop"
grep -q '^STOP' "$FAKE" && echo "ok: the shell was told to stop" || fail "no stop sent"
echo "--- 7. a failure says so in plain words"
say "Please fail this one"
wait_id act-task-review && touch "$SCRATCH/state/approve"
wait_id act-task-dismiss && echo "ok: it ended" || fail "never ended"
sleep 0.5; shot act-failed
has_id act-task-failed && echo "ok: the card says it did not work" || fail "the card does not show a failure"
EXPECTED='SUBMIT "Make the screen dimmer"
SUBMIT "Dim it again"
SUBMIT "Please fail this one"'
[ "$(grep '^SUBMIT' "$FAKE")" = "$EXPECTED" ] && echo "ok: exactly the three requests typed were handed over, as typed" || { fail "requests differ"; cat "$FAKE"; }
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
