#!/bin/bash
# Mobius's desktop tasks: a request handed to the desktop assistant is shown where it was asked, step by step, the person can stop it
# or review it, and how it ended is written into the conversation and saved with it. Approving is not something Mobius can do: the test stands in for the shell by creating a file.
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
echo "--- 4. approval in the shell finishes it: the card goes and the outcome is written into the conversation"
touch "$SCRATCH/state/approve"
wait_id task-note-0 && echo "ok: the finished task is in the thread" || fail "no task note in the thread"
gone_id act-task && echo "ok: the progress card is gone" || fail "the card stayed after the task ended"
grep -q APPROVED "$FAKE" && echo "ok: approved in the shell" || fail "not approved"
shot act-done
echo "--- 5. it is in the saved chat, with the request and the outcome"
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 20); do ls "$CHATS"/*.json >/dev/null 2>&1 && break; sleep 0.25; done
F=$(ls "$CHATS"/*.json 2>/dev/null | head -1)
tasks() { python3 - "$F" <<'PY'
import json, sys
try: d = json.load(open(sys.argv[1]))
except Exception: sys.exit(0)
print(",".join(f"{t['outcome']}:{t['request']}" for t in d.get("tasks", [])))
PY
}
[ "$(tasks)" = "done:Make the screen dimmer" ] && echo "ok: saved: done, with the request" || fail "saved tasks: $(tasks)"
[ "$(stat -c %a "$F" 2>/dev/null)" = 600 ] && echo "ok: the file is owner only" || fail "file mode: $(stat -c %a "$F" 2>/dev/null)"
python3 - "$F" <<'PY' && echo "ok: the model is not sent the task text (no message was created for it)" || fail "a message was created"
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if d.get("messages") == [] else 1)
PY
echo "--- 5b. the note offers Undo while the shell still can; it writes the previous value back and the note says so"
wait_id task-note-undo-0 && echo "ok: Undo is offered on the finished task" || fail "no Undo on the note"
click_id task-note-undo-0; sleep 1
grep -q '^UNDO 1$' "$FAKE" && echo "ok: the shell was asked to undo task 1" || fail "no undo sent: $(cat "$FAKE")"
gone_id task-note-undo-0 && echo "ok: Undo is gone once used" || fail "Undo stayed"
shot act-undone
[ "$(grep -c '^APPROVED' "$FAKE")" = 1 ] && echo "ok: undoing needed no approval" || fail "approval count changed"
python3 - "$F" <<'PY' && echo "ok: the saved task records the restore" || fail "saved task is not marked undone"
import json, sys
d = json.load(open(sys.argv[1]))
t = d["tasks"][0]
sys.exit(0 if t.get("undone") and t.get("before") == "80%" and t.get("after_text") == "40%" else 1)
PY
echo "--- 6. Stop cancels a task in progress and is recorded as stopped"
say "Dim it again"
wait_id act-task-stop && click_id act-task-stop; gone_id act-task && echo "ok: stopped, the card went away" || fail "did not stop"
grep -q '^STOP' "$FAKE" && echo "ok: the shell was told to stop" || fail "no stop sent"
wait_id task-note-1 && echo "ok: the stop is in the thread" || fail "no note for the stopped task"
echo "--- 7. a failure is recorded as failed, in plain words"
say "Please fail this one"
wait_id act-task-review && touch "$SCRATCH/state/approve"
wait_id task-note-2 && echo "ok: the failure is in the thread" || fail "no note for the failed task"
sleep 0.5; shot act-failed
[ "$(tasks)" = "done:Make the screen dimmer,stopped:Dim it again,failed:Please fail this one" ] && echo "ok: all three saved, in order, with their outcomes" || fail "saved tasks: $(tasks)"
EXPECTED='SUBMIT "Make the screen dimmer"
SUBMIT "Dim it again"
SUBMIT "Please fail this one"'
[ "$(grep '^SUBMIT' "$FAKE")" = "$EXPECTED" ] && echo "ok: exactly the three requests typed were handed over, as typed" || { fail "requests differ"; cat "$FAKE"; }
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
